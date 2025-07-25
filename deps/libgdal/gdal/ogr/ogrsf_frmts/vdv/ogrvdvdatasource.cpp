/******************************************************************************
 *
 * Project:  VDV Translator
 * Purpose:  Implements OGRVDVFDriver.
 * Author:   Even Rouault, even.rouault at spatialys.com
 *
 ******************************************************************************
 * Copyright (c) 2015, Even Rouault <even.rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_vdv.h"
#include "cpl_conv.h"
#include "cpl_time.h"

#include "memdataset.h"

#include <map>

#ifdef EMBED_RESOURCE_FILES
#include "embedded_resources.h"
#endif

#ifndef STARTS_WITH_CI
#define STARTS_WITH(a, b) (strncmp(a, b, strlen(b)) == 0)
#define STARTS_WITH_CI(a, b) EQUALN(a, b, strlen(b))
#endif

typedef enum
{
    LAYER_OTHER,
    LAYER_NODE,
    LAYER_LINK,
    LAYER_LINKCOORDINATE
} IDFLayerType;

/************************************************************************/
/*                          OGRVDVParseAtrFrm()                         */
/************************************************************************/

static void OGRVDVParseAtrFrm(OGRLayer *poLayer, OGRFeatureDefn *poFeatureDefn,
                              char **papszAtr, char **papszFrm)
{
    for (int i = 0; papszAtr[i]; i++)
    {
        OGRFieldType eType = OFTString;
        int nWidth = 0;
        OGRFieldSubType eSubType = OFSTNone;
        if (STARTS_WITH_CI(papszFrm[i], "decimal"))
        {
            if (papszFrm[i][strlen("decimal")] == '(')
            {
                if (strchr(papszFrm[i], ',') &&
                    atoi(strchr(papszFrm[i], ',') + 1) > 0)
                {
                    eType = OFTReal;
                }
                else
                {
                    nWidth = atoi(papszFrm[i] + strlen("decimal") + 1);
                    if (nWidth >= 10)
                        eType = OFTInteger64;
                    else
                        eType = OFTInteger;
                }
            }
            else
                eType = OFTInteger;
        }
        else if (STARTS_WITH_CI(papszFrm[i], "num"))
        {
            if (papszFrm[i][strlen("num")] == '[')
            {
                if (strchr(papszFrm[i], '.') &&
                    atoi(strchr(papszFrm[i], '.') + 1) > 0)
                {
                    eType = OFTReal;
                }
                else
                {
                    nWidth = atoi(papszFrm[i] + strlen("num") + 1);
                    if (nWidth < 0 || nWidth >= 100)
                    {
                        nWidth = 0;
                        eType = OFTInteger;
                    }
                    else
                    {
                        nWidth += 1; /* VDV-451 width is without sign */
                        if (nWidth >= 10)
                            eType = OFTInteger64;
                        else
                            eType = OFTInteger;
                    }
                }
            }
            else
                eType = OFTInteger;
        }
        else if (STARTS_WITH_CI(papszFrm[i], "char"))
        {
            if (papszFrm[i][strlen("char")] == '[')
            {
                nWidth = atoi(papszFrm[i] + strlen("char") + 1);
                if (nWidth < 0)
                    nWidth = 0;
            }
        }
        else if (STARTS_WITH_CI(papszFrm[i], "boolean"))
        {
            eType = OFTInteger;
            eSubType = OFSTBoolean;
        }
        OGRFieldDefn oFieldDefn(papszAtr[i], eType);
        oFieldDefn.SetSubType(eSubType);
        oFieldDefn.SetWidth(nWidth);
        if (poLayer)
            poLayer->CreateField(&oFieldDefn);
        else if (poFeatureDefn)
            poFeatureDefn->AddFieldDefn(&oFieldDefn);
        else
        {
            CPLAssert(false);
        }
    }
}

/************************************************************************/
/*                           OGRIDFDataSource()                         */
/************************************************************************/

OGRIDFDataSource::OGRIDFDataSource(const char *pszFilename, VSILFILE *fpLIn)
    : m_osFilename(pszFilename), m_fpL(fpLIn), m_bHasParsed(false),
      m_poTmpDS(nullptr)
{
}

/************************************************************************/
/*                          ~OGRIDFDataSource()                         */
/************************************************************************/

OGRIDFDataSource::~OGRIDFDataSource()
{
    CPLString osTmpFilename;
    if (m_bDestroyTmpDS && m_poTmpDS)
    {
        osTmpFilename = m_poTmpDS->GetDescription();
    }
    delete m_poTmpDS;
    if (m_bDestroyTmpDS)
    {
        VSIUnlink(osTmpFilename);
    }
    if (m_fpL)
    {
        VSIFCloseL(m_fpL);
    }
}

/************************************************************************/
/*                                Parse()                               */
/************************************************************************/

void OGRIDFDataSource::Parse()
{
    m_bHasParsed = true;

    VSIStatBufL sStatBuf;
    bool bGPKG = false;
    vsi_l_offset nFileSize = 0;
    bool bSpatialIndex = false;
    if (VSIStatL(m_osFilename, &sStatBuf) == 0 &&
        sStatBuf.st_size > CPLAtoGIntBig(CPLGetConfigOption(
                               "OGR_IDF_TEMP_DB_THRESHOLD", "100000000")))
    {
        nFileSize = sStatBuf.st_size;

        GDALDriver *poGPKGDriver =
            reinterpret_cast<GDALDriver *>(GDALGetDriverByName("GPKG"));
        if (poGPKGDriver)
        {
            CPLString osTmpFilename(m_osFilename + "_tmp.gpkg");
            VSILFILE *fp = VSIFOpenL(osTmpFilename, "wb");
            if (fp)
            {
                VSIFCloseL(fp);
            }
            else
            {
                osTmpFilename = CPLGenerateTempFilenameSafe(
                    CPLGetBasenameSafe(m_osFilename).c_str());
                osTmpFilename += ".gpkg";
            }
            VSIUnlink(osTmpFilename);
            {
                CPLConfigOptionSetter oSetter1("OGR_SQLITE_JOURNAL", "OFF",
                                               false);
                // For use of OGR VSI-based SQLite3 VFS implementation, as
                // the regular SQLite3 implementation has some issues to deal
                // with a file that is deleted after having been created.
                // For example on MacOS Big Sur system's sqlite 3.32.3
                // when chaining ogr_sqlite.py and ogr_vdv.py, or in Vagrant
                // Ubuntu 22.04 environment with sqlite 3.37.2
                CPLConfigOptionSetter oSetter2("SQLITE_USE_OGR_VFS", "YES",
                                               false);
                m_poTmpDS = poGPKGDriver->Create(osTmpFilename, 0, 0, 0,
                                                 GDT_Unknown, nullptr);
            }
            bGPKG = m_poTmpDS != nullptr;
            m_bDestroyTmpDS = CPLTestBool(CPLGetConfigOption(
                                  "OGR_IDF_DELETE_TEMP_DB", "YES")) &&
                              m_poTmpDS != nullptr;
            if (m_bDestroyTmpDS)
            {
                CPLPushErrorHandler(CPLQuietErrorHandler);
                m_bDestroyTmpDS = VSIUnlink(osTmpFilename) != 0;
                CPLPopErrorHandler();
            }
            else
            {
                bSpatialIndex = true;
            }
        }
    }

    bool bIsMEMLayer = false;
    if (m_poTmpDS == nullptr)
    {
        bIsMEMLayer = true;
        m_poTmpDS = MEMDataset::Create("", 0, 0, 0, GDT_Unknown, nullptr);
    }

    m_poTmpDS->StartTransaction();

    OGRLayer *poCurLayer = nullptr;

    struct Point
    {
        double x;
        double y;
        double z;

        explicit Point(double xIn = 0, double yIn = 0, double zIn = 0)
            : x(xIn), y(yIn), z(zIn)
        {
        }
    };

    std::map<GIntBig, Point> oMapNode;  // map from NODE_ID to Point
    std::map<GIntBig, OGRLineString *>
        oMapLinkCoordinate;  // map from LINK_ID to OGRLineString*
    CPLString osTablename, osAtr, osFrm;
    int iX = -1, iY = -1, iZ = -1;
    bool bAdvertiseUTF8 = false;
    bool bRecodeFromLatin1 = false;
    int iNodeID = -1;
    int iLinkID = -1;
    int iFromNode = -1;
    int iToNode = -1;
    IDFLayerType eLayerType = LAYER_OTHER;

    // We assume that layers are in the order Node, Link, LinkCoordinate

    GUIntBig nLineCount = 0;
    while (true)
    {
        if (nFileSize)
        {
            ++nLineCount;
            if ((nLineCount % 32768) == 0)
            {
                const vsi_l_offset nPos = VSIFTellL(m_fpL);
                CPLDebug("IDF", "Reading progress: %.2f %%",
                         100.0 * nPos / nFileSize);
            }
        }

        const char *pszLine = CPLReadLineL(m_fpL);
        if (pszLine == nullptr)
            break;

        if (strcmp(pszLine, "chs;ISO_LATIN_1") == 0)
        {
            bAdvertiseUTF8 = true;
            bRecodeFromLatin1 = true;
        }
        else if (STARTS_WITH(pszLine, "tbl;"))
        {
            poCurLayer = nullptr;
            osTablename = pszLine + 4;
            osAtr = "";
            osFrm = "";
            iX = iY = iNodeID = iLinkID = iFromNode = iToNode = -1;
            eLayerType = LAYER_OTHER;
        }
        else if (STARTS_WITH(pszLine, "atr;"))
        {
            osAtr = pszLine + 4;
            osAtr.Trim();
        }
        else if (STARTS_WITH(pszLine, "frm;"))
        {
            osFrm = pszLine + 4;
            osFrm.Trim();
        }
        else if (STARTS_WITH(pszLine, "rec;"))
        {
            if (poCurLayer == nullptr)
            {
                char **papszAtr = CSLTokenizeString2(osAtr, ";",
                                                     CSLT_ALLOWEMPTYTOKENS |
                                                         CSLT_STRIPLEADSPACES |
                                                         CSLT_STRIPENDSPACES);
                char **papszFrm = CSLTokenizeString2(osFrm, ";",
                                                     CSLT_ALLOWEMPTYTOKENS |
                                                         CSLT_STRIPLEADSPACES |
                                                         CSLT_STRIPENDSPACES);
                char *apszOptions[2] = {nullptr, nullptr};
                if (bAdvertiseUTF8 && !bGPKG)
                    apszOptions[0] = (char *)"ADVERTIZE_UTF8=YES";
                else if (bGPKG && !bSpatialIndex)
                    apszOptions[0] = (char *)"SPATIAL_INDEX=NO";

                if (EQUAL(osTablename, "Node") &&
                    (iX = CSLFindString(papszAtr, "X")) >= 0 &&
                    (iY = CSLFindString(papszAtr, "Y")) >= 0)
                {
                    iZ = CSLFindString(papszAtr, "Z");
                    eLayerType = LAYER_NODE;
                    iNodeID = CSLFindString(papszAtr, "NODE_ID");
                    OGRSpatialReference *poSRS =
                        new OGRSpatialReference(SRS_WKT_WGS84_LAT_LONG);
                    poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                    poCurLayer = m_poTmpDS->CreateLayer(
                        osTablename, poSRS, iZ < 0 ? wkbPoint : wkbPoint25D,
                        apszOptions);
                    poSRS->Release();
                }
                else if (EQUAL(osTablename, "Link") &&
                         (iLinkID = CSLFindString(papszAtr, "LINK_ID")) >= 0 &&
                         ((iFromNode = CSLFindString(papszAtr, "FROM_NODE")) >=
                          0) &&
                         ((iToNode = CSLFindString(papszAtr, "TO_NODE")) >= 0))
                {
                    eLayerType = LAYER_LINK;
                    OGRSpatialReference *poSRS =
                        new OGRSpatialReference(SRS_WKT_WGS84_LAT_LONG);
                    poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                    poCurLayer = m_poTmpDS->CreateLayer(
                        osTablename, poSRS,
                        iZ < 0 ? wkbLineString : wkbLineString25D, apszOptions);
                    poSRS->Release();
                }
                else if (EQUAL(osTablename, "LinkCoordinate") &&
                         (iLinkID = CSLFindString(papszAtr, "LINK_ID")) >= 0 &&
                         CSLFindString(papszAtr, "COUNT") >= 0 &&
                         (iX = CSLFindString(papszAtr, "X")) >= 0 &&
                         (iY = CSLFindString(papszAtr, "Y")) >= 0)
                {
                    iZ = CSLFindString(papszAtr, "Z");
                    eLayerType = LAYER_LINKCOORDINATE;
                    OGRSpatialReference *poSRS =
                        new OGRSpatialReference(SRS_WKT_WGS84_LAT_LONG);
                    poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                    poCurLayer = m_poTmpDS->CreateLayer(
                        osTablename, poSRS, iZ < 0 ? wkbPoint : wkbPoint25D,
                        apszOptions);
                    poSRS->Release();
                }
                else
                {
                    poCurLayer = m_poTmpDS->CreateLayer(osTablename, nullptr,
                                                        wkbNone, apszOptions);
                }
                if (poCurLayer == nullptr)
                {
                    CSLDestroy(papszAtr);
                    CSLDestroy(papszFrm);
                    break;
                }

                if (!osAtr.empty() && CSLCount(papszAtr) == CSLCount(papszFrm))
                {
                    OGRVDVParseAtrFrm(poCurLayer, nullptr, papszAtr, papszFrm);
                }
                CSLDestroy(papszAtr);
                CSLDestroy(papszFrm);
            }

            OGRErr eErr = OGRERR_NONE;
            char **papszTokens =
                CSLTokenizeStringComplex(pszLine + 4, ";", TRUE, TRUE);
            OGRFeatureDefn *poFDefn = poCurLayer->GetLayerDefn();
            OGRFeature *poFeature = new OGRFeature(poFDefn);
            for (int i = 0;
                 i < poFDefn->GetFieldCount() && papszTokens[i] != nullptr; i++)
            {
                if (papszTokens[i][0])
                {
                    if (bRecodeFromLatin1 &&
                        poFDefn->GetFieldDefn(i)->GetType() == OFTString)
                    {
                        char *pszRecoded = CPLRecode(
                            papszTokens[i], CPL_ENC_ISO8859_1, CPL_ENC_UTF8);
                        poFeature->SetField(i, pszRecoded);
                        CPLFree(pszRecoded);
                    }
                    else
                    {
                        poFeature->SetField(i, papszTokens[i]);
                    }
                }
            }

            if (eLayerType == LAYER_NODE && iX >= 0 && iY >= 0 && iNodeID >= 0)
            {
                double dfX = poFeature->GetFieldAsDouble(iX);
                double dfY = poFeature->GetFieldAsDouble(iY);
                OGRGeometry *poGeom;
                if (iZ >= 0)
                {
                    double dfZ = poFeature->GetFieldAsDouble(iZ);
                    oMapNode[poFeature->GetFieldAsInteger64(iNodeID)] =
                        Point(dfX, dfY, dfZ);
                    poGeom = new OGRPoint(dfX, dfY, dfZ);
                }
                else
                {
                    oMapNode[poFeature->GetFieldAsInteger64(iNodeID)] =
                        Point(dfX, dfY);
                    poGeom = new OGRPoint(dfX, dfY);
                }
                poGeom->assignSpatialReference(
                    poFDefn->GetGeomFieldDefn(0)->GetSpatialRef());
                poFeature->SetGeometryDirectly(poGeom);
            }
            else if (eLayerType == LAYER_LINK && iFromNode >= 0 && iToNode >= 0)
            {
                GIntBig nFromNode = poFeature->GetFieldAsInteger64(iFromNode);
                GIntBig nToNode = poFeature->GetFieldAsInteger64(iToNode);
                std::map<GIntBig, Point>::iterator oIterFrom =
                    oMapNode.find(nFromNode);
                std::map<GIntBig, Point>::iterator oIterTo =
                    oMapNode.find(nToNode);
                if (oIterFrom != oMapNode.end() && oIterTo != oMapNode.end())
                {
                    OGRLineString *poLS = new OGRLineString();
                    if (iZ >= 0)
                    {
                        poLS->addPoint(oIterFrom->second.x, oIterFrom->second.y,
                                       oIterFrom->second.z);
                        poLS->addPoint(oIterTo->second.x, oIterTo->second.y,
                                       oIterTo->second.z);
                    }
                    else
                    {
                        poLS->addPoint(oIterFrom->second.x,
                                       oIterFrom->second.y);
                        poLS->addPoint(oIterTo->second.x, oIterTo->second.y);
                    }
                    poLS->assignSpatialReference(
                        poFDefn->GetGeomFieldDefn(0)->GetSpatialRef());
                    poFeature->SetGeometryDirectly(poLS);
                }
            }
            else if (eLayerType == LAYER_LINKCOORDINATE && iX >= 0 && iY >= 0 &&
                     iLinkID >= 0)
            {
                double dfX = poFeature->GetFieldAsDouble(iX);
                double dfY = poFeature->GetFieldAsDouble(iY);
                double dfZ = 0.0;
                OGRGeometry *poGeom;
                if (iZ >= 0)
                {
                    dfZ = poFeature->GetFieldAsDouble(iZ);
                    poGeom = new OGRPoint(dfX, dfY, dfZ);
                }
                else
                {
                    poGeom = new OGRPoint(dfX, dfY);
                }
                poGeom->assignSpatialReference(
                    poFDefn->GetGeomFieldDefn(0)->GetSpatialRef());
                poFeature->SetGeometryDirectly(poGeom);

                GIntBig nCurLinkID = poFeature->GetFieldAsInteger64(iLinkID);
                std::map<GIntBig, OGRLineString *>::iterator
                    oMapLinkCoordinateIter =
                        oMapLinkCoordinate.find(nCurLinkID);
                if (oMapLinkCoordinateIter == oMapLinkCoordinate.end())
                {
                    OGRLineString *poLS = new OGRLineString();
                    if (iZ >= 0)
                        poLS->addPoint(dfX, dfY, dfZ);
                    else
                        poLS->addPoint(dfX, dfY);
                    oMapLinkCoordinate[nCurLinkID] = poLS;
                }
                else
                {
                    if (iZ >= 0)
                    {
                        oMapLinkCoordinateIter->second->addPoint(dfX, dfY, dfZ);
                    }
                    else
                    {
                        oMapLinkCoordinateIter->second->addPoint(dfX, dfY);
                    }
                }
            }
            eErr = poCurLayer->CreateFeature(poFeature);
            delete poFeature;

            CSLDestroy(papszTokens);

            if (eErr == OGRERR_FAILURE)
                break;
        }
    }

    oMapNode.clear();

    // Patch Link geometries with the intermediate points of LinkCoordinate
    OGRLayer *poLinkLyr = m_poTmpDS->GetLayerByName("Link");
    if (poLinkLyr && poLinkLyr->GetLayerDefn()->GetGeomFieldCount())
    {
        iLinkID = poLinkLyr->GetLayerDefn()->GetFieldIndex("LINK_ID");
        if (iLinkID >= 0)
        {
            poLinkLyr->ResetReading();
            const OGRSpatialReference *poSRS =
                poLinkLyr->GetLayerDefn()->GetGeomFieldDefn(0)->GetSpatialRef();
            for (auto &&poFeat : poLinkLyr)
            {
                GIntBig nLinkID = poFeat->GetFieldAsInteger64(iLinkID);
                std::map<GIntBig, OGRLineString *>::iterator
                    oMapLinkCoordinateIter = oMapLinkCoordinate.find(nLinkID);
                OGRGeometry *poGeom = poFeat->GetGeometryRef();
                if (poGeom &&
                    oMapLinkCoordinateIter != oMapLinkCoordinate.end())
                {
                    OGRLineString *poLS = poGeom->toLineString();
                    if (poLS)
                    {
                        OGRLineString *poLSIntermediate =
                            oMapLinkCoordinateIter->second;
                        OGRLineString *poLSNew = new OGRLineString();
                        if (poLS->getGeometryType() == wkbLineString25D)
                        {
                            poLSNew->addPoint(poLS->getX(0), poLS->getY(0),
                                              poLS->getZ(0));
                            for (int i = 0;
                                 i < poLSIntermediate->getNumPoints(); i++)
                            {
                                poLSNew->addPoint(poLSIntermediate->getX(i),
                                                  poLSIntermediate->getY(i),
                                                  poLSIntermediate->getZ(i));
                            }
                            poLSNew->addPoint(poLS->getX(1), poLS->getY(1),
                                              poLS->getZ(1));
                        }
                        else
                        {
                            poLSNew->addPoint(poLS->getX(0), poLS->getY(0));
                            for (int i = 0;
                                 i < poLSIntermediate->getNumPoints(); i++)
                            {
                                poLSNew->addPoint(poLSIntermediate->getX(i),
                                                  poLSIntermediate->getY(i));
                            }
                            poLSNew->addPoint(poLS->getX(1), poLS->getY(1));
                        }
                        poLSNew->assignSpatialReference(poSRS);
                        poFeat->SetGeometryDirectly(poLSNew);
                        CPL_IGNORE_RET_VAL(poLinkLyr->SetFeature(poFeat.get()));
                    }
                }
            }
            poLinkLyr->ResetReading();
        }
    }

    m_poTmpDS->CommitTransaction();

    if (bIsMEMLayer)
        m_poTmpDS->ExecuteSQL("PRAGMA read_only=1", nullptr, nullptr);

    std::map<GIntBig, OGRLineString *>::iterator oMapLinkCoordinateIter =
        oMapLinkCoordinate.begin();
    for (; oMapLinkCoordinateIter != oMapLinkCoordinate.end();
         ++oMapLinkCoordinateIter)
        delete oMapLinkCoordinateIter->second;
}

/************************************************************************/
/*                           GetLayerCount()                            */
/************************************************************************/

int OGRIDFDataSource::GetLayerCount()
{
    if (!m_bHasParsed)
        Parse();
    if (m_poTmpDS == nullptr)
        return 0;
    return m_poTmpDS->GetLayerCount();
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRIDFDataSource::GetLayer(int iLayer)
{
    if (iLayer < 0 || iLayer >= GetLayerCount())
        return nullptr;
    if (m_poTmpDS == nullptr)
        return nullptr;
    return m_poTmpDS->GetLayer(iLayer);
}

/************************************************************************/
/*                              TestCapability()                        */
/************************************************************************/

int OGRIDFDataSource::TestCapability(const char *pszCap)
{
    if (EQUAL(pszCap, ODsCMeasuredGeometries))
        return true;
    else if (EQUAL(pszCap, ODsCCurveGeometries))
        return true;
    else if (EQUAL(pszCap, ODsCZGeometries))
        return true;

    return false;
}

/************************************************************************/
/*                           OGRVDVDataSource()                         */
/************************************************************************/

OGRVDVDataSource::OGRVDVDataSource(const char *pszFilename, VSILFILE *fpL,
                                   bool bUpdate, bool bSingleFile, bool bNew)
    : m_osFilename(pszFilename), m_fpL(fpL), m_bUpdate(bUpdate),
      m_bSingleFile(bSingleFile), m_bNew(bNew),
      m_bLayersDetected(bNew || fpL == nullptr), m_nLayerCount(0),
      m_papoLayers(nullptr), m_poCurrentWriterLayer(nullptr),
      m_bMustWriteEof(false), m_bVDV452Loaded(false)
{
}

/************************************************************************/
/*                          ~OGRVDVDataSource()                         */
/************************************************************************/

OGRVDVDataSource::~OGRVDVDataSource()
{
    if (m_poCurrentWriterLayer)
    {
        m_poCurrentWriterLayer->StopAsCurrentLayer();
        m_poCurrentWriterLayer = nullptr;
    }

    for (int i = 0; i < m_nLayerCount; i++)
        delete m_papoLayers[i];
    CPLFree(m_papoLayers);

    // Close after destroying layers since they might use it (single file write)
    if (m_fpL)
    {
        if (m_bMustWriteEof)
        {
            VSIFPrintfL(m_fpL, "eof; %d\n", m_nLayerCount);
        }
        VSIFCloseL(m_fpL);
    }
}

/************************************************************************/
/*                           GetLayerCount()                            */
/************************************************************************/

int OGRVDVDataSource::GetLayerCount()
{
    if (!m_bLayersDetected)
        DetectLayers();
    return m_nLayerCount;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRVDVDataSource::GetLayer(int iLayer)
{
    if (iLayer < 0 || iLayer >= GetLayerCount())
        return nullptr;
    return m_papoLayers[iLayer];
}

/************************************************************************/
/*                         DetectLayers()                               */
/************************************************************************/

void OGRVDVDataSource::DetectLayers()
{
    m_bLayersDetected = true;

    char szBuffer[1 + 1024 + 1];
    char chNextExpected = 't';
    char chNextExpected2 = 'r';
    char chNextExpected3 = 'e';
    bool bInTableName = false;
    CPLString osTableName;
    GIntBig nFeatureCount = 0;
    vsi_l_offset nStartOffset = 0;
    OGRVDVLayer *poLayer = nullptr;
    bool bFirstBuffer = true;
    bool bRecodeFromLatin1 = false;

    VSIFSeekL(m_fpL, 0, SEEK_SET);

    while (true)
    {
        size_t nRead = VSIFReadL(szBuffer, 1, 1024, m_fpL);
        szBuffer[nRead] = '\0';
        if (bFirstBuffer)
        {
            const char *pszChs = strstr(szBuffer, "\nchs;");
            if (pszChs)
            {
                pszChs += 5;
                CPLString osChs;
                for (; *pszChs != '\0' && *pszChs != '\r' && *pszChs != '\n';
                     ++pszChs)
                {
                    if (*pszChs != ' ' && *pszChs != '"')
                        osChs += *pszChs;
                }
                bRecodeFromLatin1 =
                    EQUAL(osChs, "ISO8859-1") || EQUAL(osChs, "ISO_LATIN_1");
            }
            bFirstBuffer = false;
        }
        for (size_t i = 0; i < nRead; i++)
        {
            if (bInTableName)
            {
                if (szBuffer[i] == '\r' || szBuffer[i] == '\n')
                {
                    bInTableName = false;
                    poLayer = new OGRVDVLayer(this, osTableName, m_fpL, false,
                                              bRecodeFromLatin1, nStartOffset);
                    m_papoLayers = static_cast<OGRLayer **>(
                        CPLRealloc(m_papoLayers,
                                   sizeof(OGRLayer *) * (m_nLayerCount + 1)));
                    m_papoLayers[m_nLayerCount] = poLayer;
                    m_nLayerCount++;
                }
                else if (szBuffer[i] != ' ')
                {
                    osTableName += szBuffer[i];
                    continue;
                }
            }

            // Reset state on end of line characters
            if (szBuffer[i] == '\n' || szBuffer[i] == '\r')
            {
                chNextExpected = szBuffer[i];
                chNextExpected2 = szBuffer[i];
                chNextExpected3 = szBuffer[i];
            }

            // Detect tbl;
            if (szBuffer[i] == chNextExpected)
            {
                if (chNextExpected == '\n' || chNextExpected == '\r')
                    chNextExpected = 't';
                else if (chNextExpected == 't')
                    chNextExpected = 'b';
                else if (chNextExpected == 'b')
                    chNextExpected = 'l';
                else if (chNextExpected == 'l')
                    chNextExpected = ';';
                else if (chNextExpected == ';')
                {
                    if (poLayer != nullptr)
                        poLayer->SetFeatureCount(nFeatureCount);
                    poLayer = nullptr;
                    nFeatureCount = 0;
                    nStartOffset = VSIFTellL(m_fpL) + i + 1 - nRead - 4;
                    bInTableName = true;
                    osTableName.resize(0);
                    chNextExpected = 0;
                }
            }
            else
                chNextExpected = 0;

            // Detect rec;
            if (szBuffer[i] == chNextExpected2)
            {
                if (chNextExpected2 == '\n' || chNextExpected2 == '\r')
                    chNextExpected2 = 'r';
                else if (chNextExpected2 == 'r')
                    chNextExpected2 = 'e';
                else if (chNextExpected2 == 'e')
                    chNextExpected2 = 'c';
                else if (chNextExpected2 == 'c')
                    chNextExpected2 = ';';
                else if (chNextExpected2 == ';')
                {
                    nFeatureCount++;
                    chNextExpected2 = 0;
                }
            }
            else
                chNextExpected2 = 0;

            // Detect end;
            if (szBuffer[i] == chNextExpected3)
            {
                if (chNextExpected3 == '\n' || chNextExpected3 == '\r')
                    chNextExpected3 = 'e';
                else if (chNextExpected3 == 'e')
                    chNextExpected3 = 'n';
                else if (chNextExpected3 == 'n')
                    chNextExpected3 = 'd';
                else if (chNextExpected3 == 'd')
                    chNextExpected3 = ';';
                else if (chNextExpected3 == ';')
                {
                    if (poLayer != nullptr)
                        poLayer->SetFeatureCount(nFeatureCount);
                    poLayer = nullptr;
                    chNextExpected3 = 0;
                }
            }
            else
                chNextExpected3 = 0;
        }
        if (nRead < 1024)
            break;
    }
    if (poLayer != nullptr)
        poLayer->SetFeatureCount(nFeatureCount);
}

/************************************************************************/
/*                           OGRVDVLayer()                              */
/************************************************************************/

OGRVDVLayer::OGRVDVLayer(GDALDataset *poDS, const CPLString &osTableName,
                         VSILFILE *fpL, bool bOwnFP, bool bRecodeFromLatin1,
                         vsi_l_offset nStartOffset)
    : m_poDS(poDS), m_fpL(fpL), m_bOwnFP(bOwnFP),
      m_bRecodeFromLatin1(bRecodeFromLatin1), m_nStartOffset(nStartOffset),
      m_nCurOffset(0), m_nTotalFeatureCount(0), m_nFID(0), m_bEOF(false),
      m_iLongitudeVDV452(-1), m_iLatitudeVDV452(-1)
{
    m_poFeatureDefn = new OGRFeatureDefn(osTableName);
    m_poFeatureDefn->SetGeomType(wkbNone);
    m_poFeatureDefn->Reference();
    SetDescription(osTableName);
    vsi_l_offset nCurOffset = VSIFTellL(fpL);
    VSIFSeekL(m_fpL, m_nStartOffset, SEEK_SET);
    CPLString osAtr, osFrm;

    /* skip until first tbl; */
    bool bFoundTbl = false;
    for (int i = 0; i < 20; i++)
    {
        const char *pszLine = CPLReadLineL(m_fpL);
        if (pszLine == nullptr)
            break;
        if (STARTS_WITH(pszLine, "chs;"))
        {
            CPLString osChs(pszLine + 4);
            osChs.Trim();
            if (osChs.size() >= 2 && osChs[0] == '"' && osChs.back() == '"')
                osChs = osChs.substr(1, osChs.size() - 2);
            m_bRecodeFromLatin1 =
                EQUAL(osChs, "ISO8859-1") || EQUAL(osChs, "ISO_LATIN_1");
        }
        else if (STARTS_WITH(pszLine, "tbl;"))
        {
            if (bFoundTbl)
                break; /* shouldn't happen in correctly formed files */
            bFoundTbl = true;
            m_nStartOffset = VSIFTellL(fpL);
        }
        else if (STARTS_WITH(pszLine, "atr;"))
        {
            osAtr = pszLine + 4;
            osAtr.Trim();
        }
        else if (STARTS_WITH(pszLine, "frm;"))
        {
            osFrm = pszLine + 4;
            osFrm.Trim();
        }
        else if (STARTS_WITH(pszLine, "rec;") || STARTS_WITH(pszLine, "end;"))
            break;
    }
    if (!bFoundTbl)
        CPLDebug("VDV", "Didn't find tbl; line");

    VSIFSeekL(m_fpL, nCurOffset, SEEK_SET);
    if (!osAtr.empty() && !osFrm.empty())
    {
        char **papszAtr = CSLTokenizeString2(
            osAtr, ";",
            CSLT_ALLOWEMPTYTOKENS | CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES);
        char **papszFrm = CSLTokenizeString2(
            osFrm, ";",
            CSLT_ALLOWEMPTYTOKENS | CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES);
        if (CSLCount(papszAtr) == CSLCount(papszFrm))
        {
            OGRVDVParseAtrFrm(nullptr, m_poFeatureDefn, papszAtr, papszFrm);
        }
        CSLDestroy(papszAtr);
        CSLDestroy(papszFrm);
    }

    // Identify longitude, latitude columns of VDV-452 STOP table
    if (EQUAL(osTableName, "STOP")) /* English */
    {
        m_iLongitudeVDV452 = m_poFeatureDefn->GetFieldIndex("POINT_LONGITUDE");
        m_iLatitudeVDV452 = m_poFeatureDefn->GetFieldIndex("POINT_LATITUDE");
    }
    else if (EQUAL(osTableName, "REC_ORT")) /* German */
    {
        m_iLongitudeVDV452 = m_poFeatureDefn->GetFieldIndex("ORT_POS_LAENGE");
        m_iLatitudeVDV452 = m_poFeatureDefn->GetFieldIndex("ORT_POS_BREITE");
    }
    if (m_iLongitudeVDV452 >= 0 && m_iLatitudeVDV452 >= 0)
    {
        m_poFeatureDefn->SetGeomType(wkbPoint);
        OGRSpatialReference *poSRS =
            new OGRSpatialReference(SRS_WKT_WGS84_LAT_LONG);
        poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        m_poFeatureDefn->GetGeomFieldDefn(0)->SetSpatialRef(poSRS);
        poSRS->Release();
    }
    else
        m_iLongitudeVDV452 = m_iLatitudeVDV452 = -1;
}

/************************************************************************/
/*                          ~OGRVDVLayer()                              */
/************************************************************************/

OGRVDVLayer::~OGRVDVLayer()
{
    m_poFeatureDefn->Release();
    if (m_bOwnFP)
        VSIFCloseL(m_fpL);
}

/************************************************************************/
/*                          ResetReading()                              */
/************************************************************************/

void OGRVDVLayer::ResetReading()
{
    VSIFSeekL(m_fpL, m_nStartOffset, SEEK_SET);
    m_nCurOffset = m_nStartOffset;
    m_nFID = 1;
    m_bEOF = false;
}

/************************************************************************/
/*                         OGRVDVUnescapeString()                       */
/************************************************************************/

static CPLString OGRVDVUnescapeString(const char *pszValue)
{
    CPLString osRet;
    for (; *pszValue != '\0'; ++pszValue)
    {
        if (*pszValue == '"' && pszValue[1] == '"')
        {
            osRet += '"';
            ++pszValue;
        }
        else
        {
            osRet += *pszValue;
        }
    }
    return osRet;
}

/************************************************************************/
/*                          GetNextFeature()                            */
/************************************************************************/

OGRFeature *OGRVDVLayer::GetNextFeature()
{
    if (m_nFID == 0)
        ResetReading();
    VSIFSeekL(m_fpL, m_nCurOffset, SEEK_SET);
    OGRFeature *poFeature = nullptr;
    while (!m_bEOF)
    {
        const char *pszLine = CPLReadLineL(m_fpL);
        if (pszLine == nullptr)
            break;
        if (strncmp(pszLine, "end;", 4) == 0 ||
            strncmp(pszLine, "tbl;", 4) == 0)
        {
            m_bEOF = true;
            break;
        }
        if (strncmp(pszLine, "rec;", 4) != 0)
            continue;

        char **papszTokens = CSLTokenizeString2(
            pszLine + 4, ";",
            CSLT_ALLOWEMPTYTOKENS | CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES);
        poFeature = new OGRFeature(m_poFeatureDefn);
        poFeature->SetFID(m_nFID++);
        for (int i = 0;
             i < m_poFeatureDefn->GetFieldCount() && papszTokens[i] != nullptr;
             i++)
        {
            if (papszTokens[i][0] && !EQUAL(papszTokens[i], "NULL"))
            {
                size_t nLen = strlen(papszTokens[i]);
                CPLString osToken;
                if (nLen >= 2 && papszTokens[i][0] == '"' &&
                    papszTokens[i][nLen - 1] == '"')
                {
                    papszTokens[i][nLen - 1] = 0;
                    osToken = OGRVDVUnescapeString(papszTokens[i] + 1);
                }
                else
                    osToken = papszTokens[i];
                // Strip trailing spaces
                while (!osToken.empty() && osToken.back() == ' ')
                    osToken.pop_back();
                OGRFieldType eFieldType =
                    m_poFeatureDefn->GetFieldDefn(i)->GetType();
                if (m_bRecodeFromLatin1 && eFieldType == OFTString)
                {
                    char *pszRecoded =
                        CPLRecode(osToken, CPL_ENC_ISO8859_1, CPL_ENC_UTF8);
                    poFeature->SetField(i, pszRecoded);
                    CPLFree(pszRecoded);
                }
                else if (eFieldType == OFTString || !EQUAL(osToken, "NULL"))
                {
                    poFeature->SetField(i, osToken);
                }
            }
        }
        CSLDestroy(papszTokens);

        if (m_iLongitudeVDV452 >= 0 && m_iLatitudeVDV452 >= 0)
        {
            int nLongDegMinMS =
                poFeature->GetFieldAsInteger(m_iLongitudeVDV452);
            int nLongSign = 1;
            if (nLongDegMinMS < 0)
            {
                nLongSign = -1;
                nLongDegMinMS = -nLongDegMinMS;
            }
            const int nLongDeg = nLongDegMinMS / (100 * 100000);
            const int nLongMin = (nLongDegMinMS / 100000) % 100;
            const int nLongMS = nLongDegMinMS % 100000;
            const double dfLong =
                (nLongDeg + nLongMin / 60.0 + nLongMS / (3600.0 * 1000.0)) *
                nLongSign;

            int nLatDegMinMS = poFeature->GetFieldAsInteger(m_iLatitudeVDV452);
            int nLatSign = 1;
            if (nLatDegMinMS < 0)
            {
                nLatSign = -1;
                nLatDegMinMS = -nLatDegMinMS;
            }
            const int nLatDeg = nLatDegMinMS / (100 * 100000);
            const int nLatMin = (nLatDegMinMS / 100000) % 100;
            const int nLatMS = nLatDegMinMS % 100000;
            const double dfLat =
                (nLatDeg + nLatMin / 60.0 + nLatMS / (3600.0 * 1000.0)) *
                nLatSign;

            if (dfLong != 0.0 || dfLat != 0.0)
            {
                OGRPoint *poPoint = new OGRPoint(dfLong, dfLat);
                poPoint->assignSpatialReference(
                    m_poFeatureDefn->GetGeomFieldDefn(0)->GetSpatialRef());
                poFeature->SetGeometryDirectly(poPoint);
            }
        }

        if ((m_poFilterGeom == nullptr ||
             FilterGeometry(poFeature->GetGeomFieldRef(m_iGeomFieldFilter))) &&
            (m_poAttrQuery == nullptr || m_poAttrQuery->Evaluate(poFeature)))
        {
            break;
        }
        delete poFeature;
        poFeature = nullptr;
    }
    m_nCurOffset = VSIFTellL(m_fpL);
    return poFeature;
}

/************************************************************************/
/*                          TestCapability()                            */
/************************************************************************/

int OGRVDVLayer::TestCapability(const char *pszCap)
{
    if (EQUAL(pszCap, OLCFastFeatureCount) && m_nTotalFeatureCount > 0 &&
        m_poFilterGeom == nullptr && m_poAttrQuery == nullptr)
    {
        return TRUE;
    }
    else if (EQUAL(pszCap, OLCStringsAsUTF8))
    {
        return m_bRecodeFromLatin1;
    }
    else if (EQUAL(pszCap, OLCZGeometries))
    {
        return TRUE;
    }
    return FALSE;
}

/************************************************************************/
/*                          GetFeatureCount()                           */
/************************************************************************/

GIntBig OGRVDVLayer::GetFeatureCount(int bForce)
{
    if (m_nTotalFeatureCount == 0 || m_poFilterGeom != nullptr ||
        m_poAttrQuery != nullptr)
    {
        return OGRLayer::GetFeatureCount(bForce);
    }
    return m_nTotalFeatureCount;
}

/************************************************************************/
/*                              Identify()                              */
/************************************************************************/

static int OGRVDVDriverIdentify(GDALOpenInfo *poOpenInfo)

{
    if (poOpenInfo->bIsDirectory)
        return -1; /* perhaps... */
    return (
        poOpenInfo->nHeaderBytes > 0 &&
        (strstr((const char *)poOpenInfo->pabyHeader, "\ntbl;") != nullptr ||
         strncmp((const char *)poOpenInfo->pabyHeader, "tbl;", 4) == 0) &&
        strstr((const char *)poOpenInfo->pabyHeader, "\natr;") != nullptr &&
        strstr((const char *)poOpenInfo->pabyHeader, "\nfrm;") != nullptr);
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *OGRVDVDataSource::Open(GDALOpenInfo *poOpenInfo)

{
    if (!OGRVDVDriverIdentify(poOpenInfo))
    {
        return nullptr;
    }
    if (poOpenInfo->bIsDirectory)
    {
        char **papszFiles = VSIReadDir(poOpenInfo->pszFilename);

        // Identify the extension with the most occurrences
        std::map<CPLString, int> oMapOtherExtensions;
        CPLString osMajorityExtension, osMajorityFile;
        int nFiles = 0;
        for (char **papszIter = papszFiles; papszIter && *papszIter;
             ++papszIter)
        {
            if (EQUAL(*papszIter, ".") || EQUAL(*papszIter, ".."))
                continue;
            nFiles++;
            const std::string osExtension(CPLGetExtensionSafe(*papszIter));
            int nCount = ++oMapOtherExtensions[osExtension];
            if (osMajorityExtension == "" ||
                nCount > oMapOtherExtensions[osMajorityExtension])
            {
                osMajorityExtension = osExtension;
                osMajorityFile = *papszIter;
            }
        }

        // Check it is at least 50% of the files in the directory
        if (osMajorityExtension == "" ||
            2 * oMapOtherExtensions[osMajorityExtension] < nFiles)
        {
            CSLDestroy(papszFiles);
            return nullptr;
        }

        // And check that one of those files is a VDV one if it isn't .x10
        if (osMajorityExtension != "x10")
        {
            GDALOpenInfo oOpenInfo(CPLFormFilenameSafe(poOpenInfo->pszFilename,
                                                       osMajorityFile, nullptr)
                                       .c_str(),
                                   GA_ReadOnly);
            if (OGRVDVDriverIdentify(&oOpenInfo) != TRUE)
            {
                CSLDestroy(papszFiles);
                return nullptr;
            }
        }

        OGRVDVDataSource *poDS = new OGRVDVDataSource(
            poOpenInfo->pszFilename, nullptr,        /* fp */
            poOpenInfo->eAccess == GA_Update, false, /* single file */
            false /* new */);

        // Instantiate the layers.
        for (char **papszIter = papszFiles; papszIter && *papszIter;
             ++papszIter)
        {
            if (!EQUAL(CPLGetExtensionSafe(*papszIter).c_str(),
                       osMajorityExtension))
                continue;
            VSILFILE *fp =
                VSIFOpenL(CPLFormFilenameSafe(poOpenInfo->pszFilename,
                                              *papszIter, nullptr)
                              .c_str(),
                          "rb");
            if (fp == nullptr)
                continue;
            poDS->m_papoLayers = static_cast<OGRLayer **>(
                CPLRealloc(poDS->m_papoLayers,
                           sizeof(OGRLayer *) * (poDS->m_nLayerCount + 1)));
            poDS->m_papoLayers[poDS->m_nLayerCount] =
                new OGRVDVLayer(poDS, CPLGetBasenameSafe(*papszIter).c_str(),
                                fp, true, false, 0);
            poDS->m_nLayerCount++;
        }
        CSLDestroy(papszFiles);

        if (poDS->m_nLayerCount == 0)
        {
            delete poDS;
            poDS = nullptr;
        }
        return poDS;
    }

    VSILFILE *fpL = poOpenInfo->fpL;
    poOpenInfo->fpL = nullptr;
    const char *pszHeader = (const char *)poOpenInfo->pabyHeader;
    if (strstr(pszHeader, "tbl;Node\r\natr;NODE_ID;") != nullptr ||
        strstr(pszHeader, "tbl;Node\natr;NODE_ID;") != nullptr ||
        strstr(pszHeader, "tbl;Link\r\natr;LINK_ID;") != nullptr ||
        strstr(pszHeader, "tbl;Link\natr;LINK_ID;") != nullptr ||
        strstr(pszHeader, "tbl;LinkCoordinate\r\natr;LINK_ID;") != nullptr ||
        strstr(pszHeader, "tbl;LinkCoordinate\natr;LINK_ID;") != nullptr)
    {
        return new OGRIDFDataSource(poOpenInfo->pszFilename, fpL);
    }
    else
    {
        return new OGRVDVDataSource(poOpenInfo->pszFilename, fpL,
                                    poOpenInfo->eAccess == GA_Update,
                                    true, /* single file */
                                    false /* new */);
    }
}

/************************************************************************/
/*                         OGRVDVWriterLayer                            */
/************************************************************************/

OGRVDVWriterLayer::OGRVDVWriterLayer(OGRVDVDataSource *poDS,
                                     const char *pszName, VSILFILE *fpL,
                                     bool bOwnFP, OGRVDV452Table *poVDV452Table,
                                     const CPLString &osVDV452Lang,
                                     bool bProfileStrict)
    : m_poDS(poDS), m_poFeatureDefn(new OGRFeatureDefn(pszName)),
      m_bWritePossible(true), m_fpL(fpL), m_bOwnFP(bOwnFP), m_nFeatureCount(-1),
      m_poVDV452Table(poVDV452Table), m_osVDV452Lang(osVDV452Lang),
      m_bProfileStrict(bProfileStrict), m_iLongitudeVDV452(-1),
      m_iLatitudeVDV452(-1)
{
    m_poFeatureDefn->SetGeomType(wkbNone);
    m_poFeatureDefn->Reference();
    SetDescription(pszName);
}

/************************************************************************/
/*                        ~OGRVDVWriterLayer                            */
/************************************************************************/

OGRVDVWriterLayer::~OGRVDVWriterLayer()
{
    StopAsCurrentLayer();

    m_poFeatureDefn->Release();
    if (m_bOwnFP)
    {
        VSIFPrintfL(m_fpL, "eof; %d\n", 1);
        VSIFCloseL(m_fpL);
    }
}

/************************************************************************/
/*                          ResetReading()                              */
/************************************************************************/

void OGRVDVWriterLayer::ResetReading()
{
}

/************************************************************************/
/*                          GetNextFeature()                            */
/************************************************************************/

OGRFeature *OGRVDVWriterLayer::GetNextFeature()
{
    CPLError(CE_Failure, CPLE_NotSupported,
             "GetNextFeature() not supported on write-only layer");
    return nullptr;
}

/************************************************************************/
/*                         OGRVDVEscapeString()                         */
/************************************************************************/

static CPLString OGRVDVEscapeString(const char *pszValue)
{
    CPLString osRet;
    for (; *pszValue != '\0'; ++pszValue)
    {
        if (*pszValue == '"')
            osRet += "\"\"";
        else
            osRet += *pszValue;
    }
    return osRet;
}

/************************************************************************/
/*                          WriteSchemaIfNeeded()                       */
/************************************************************************/

bool OGRVDVWriterLayer::WriteSchemaIfNeeded()
{
    if (m_nFeatureCount < 0)
    {
        m_nFeatureCount = 0;

        bool bOK =
            VSIFPrintfL(m_fpL, "tbl; %s\n", m_poFeatureDefn->GetName()) > 0;
        bOK &= VSIFPrintfL(m_fpL, "atr;") > 0;
        for (int i = 0; i < m_poFeatureDefn->GetFieldCount(); i++)
        {
            if (i > 0)
                bOK &= VSIFPrintfL(m_fpL, ";") > 0;
            bOK &=
                VSIFPrintfL(m_fpL, " %s",
                            m_poFeatureDefn->GetFieldDefn(i)->GetNameRef()) > 0;
        }
        bOK &= VSIFPrintfL(m_fpL, "\n") > 0;
        bOK &= VSIFPrintfL(m_fpL, "frm;") > 0;
        for (int i = 0; i < m_poFeatureDefn->GetFieldCount(); i++)
        {
            if (i > 0)
                bOK &= VSIFPrintfL(m_fpL, ";") > 0;
            bOK &= VSIFPrintfL(m_fpL, " ") > 0;
            int nWidth = m_poFeatureDefn->GetFieldDefn(i)->GetWidth();
            const OGRFieldType eType =
                m_poFeatureDefn->GetFieldDefn(i)->GetType();
            switch (eType)
            {
                case OFTInteger:
                case OFTInteger64:
                    if (m_poFeatureDefn->GetFieldDefn(i)->GetSubType() ==
                        OFSTBoolean)
                    {
                        bOK &= VSIFPrintfL(m_fpL, "boolean") > 0;
                    }
                    else
                    {
                        if (nWidth == 0)
                        {
                            if (eType == OFTInteger)
                                nWidth = 11;
                            else
                                nWidth = 20;
                        }
                        nWidth--; /* VDV 451 is without sign */
                        bOK &= VSIFPrintfL(m_fpL, "num[%d.0]", nWidth) > 0;
                    }
                    break;

                default:
                    if (nWidth == 0)
                    {
                        nWidth = 80;
                    }
                    bOK &= VSIFPrintfL(m_fpL, "char[%d]", nWidth) > 0;
                    break;
            }
        }
        bOK &= VSIFPrintfL(m_fpL, "\n") > 0;

        if (!bOK)
            return false;
    }

    return true;
}

/************************************************************************/
/*                         ICreateFeature()                             */
/************************************************************************/

OGRErr OGRVDVWriterLayer::ICreateFeature(OGRFeature *poFeature)
{
    if (!m_bWritePossible)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Layer %s is no longer the active layer. "
                 "Writing in it is no longer possible",
                 m_poFeatureDefn->GetName());
        return OGRERR_FAILURE;
    }
    m_poDS->SetCurrentWriterLayer(this);

    WriteSchemaIfNeeded();

    bool bOK = VSIFPrintfL(m_fpL, "rec; ") > 0;
    for (int i = 0; i < m_poFeatureDefn->GetFieldCount(); i++)
    {
        if (i > 0)
            bOK &= VSIFPrintfL(m_fpL, "; ") > 0;
        auto poGeom = poFeature->GetGeometryRef();
        if (poFeature->IsFieldSetAndNotNull(i))
        {
            const OGRFieldType eType =
                m_poFeatureDefn->GetFieldDefn(i)->GetType();
            if (eType == OFTInteger || eType == OFTInteger64)
            {
                bOK &= VSIFPrintfL(m_fpL, CPL_FRMT_GIB,
                                   poFeature->GetFieldAsInteger64(i)) > 0;
            }
            else
            {
                char *pszRecoded = CPLRecode(poFeature->GetFieldAsString(i),
                                             CPL_ENC_UTF8, CPL_ENC_ISO8859_1);
                bOK &= VSIFPrintfL(m_fpL, "\"%s\"",
                                   OGRVDVEscapeString(pszRecoded).c_str()) > 0;
                CPLFree(pszRecoded);
            }
        }
        else if (i == m_iLongitudeVDV452 && poGeom != nullptr &&
                 poGeom->getGeometryType() == wkbPoint)
        {
            OGRPoint *poPoint = poGeom->toPoint();
            const double dfDeg = poPoint->getX();
            const double dfAbsDeg = fabs(dfDeg);
            const int nDeg = static_cast<int>(dfAbsDeg);
            const int nMin = static_cast<int>((dfAbsDeg - nDeg) * 60);
            const double dfSec = (dfAbsDeg - nDeg) * 3600 - nMin * 60;
            const int nSec = static_cast<int>(dfSec);
            int nMS = static_cast<int>((dfSec - nSec) * 1000 + 0.5);
            if (nMS == 1000)
                nMS = 999;
            if (dfDeg < 0)
                bOK &= VSIFPrintfL(m_fpL, "-") > 0;
            bOK &= VSIFPrintfL(m_fpL, "%03d%02d%02d%03d", nDeg, nMin, nSec,
                               nMS) > 0;
        }
        else if (i == m_iLatitudeVDV452 && poGeom != nullptr &&
                 poGeom->getGeometryType() == wkbPoint)
        {
            OGRPoint *poPoint = poGeom->toPoint();
            const double dfDeg = poPoint->getY();
            const double dfAbsDeg = fabs(dfDeg);
            const int nDeg = static_cast<int>(dfAbsDeg);
            const int nMin = static_cast<int>((dfAbsDeg - nDeg) * 60);
            const double dfSec = (dfAbsDeg - nDeg) * 3600 - nMin * 60;
            const int nSec = static_cast<int>(dfSec);
            int nMS = static_cast<int>((dfSec - nSec) * 1000 + 0.5);
            if (nMS == 1000)
                nMS = 999;
            if (dfDeg < 0)
                bOK &= VSIFPrintfL(m_fpL, "-") > 0;
            bOK &= VSIFPrintfL(m_fpL, "%02d%02d%02d%03d", nDeg, nMin, nSec,
                               nMS) > 0;
        }
        else
        {
            bOK &= VSIFPrintfL(m_fpL, "NULL") > 0;
        }
    }
    bOK &= VSIFPrintfL(m_fpL, "\n") > 0;

    if (!bOK)
        return OGRERR_FAILURE;

    m_nFeatureCount++;
    return OGRERR_NONE;
}

/************************************************************************/
/*                         GetFeatureCount()                            */
/************************************************************************/

GIntBig OGRVDVWriterLayer::GetFeatureCount(int)
{
    return m_nFeatureCount >= 0 ? m_nFeatureCount : 0;
}

/************************************************************************/
/*                          CreateField()                               */
/************************************************************************/

OGRErr OGRVDVWriterLayer::CreateField(const OGRFieldDefn *poFieldDefn,
                                      int /* bApprox */)
{
    if (m_nFeatureCount >= 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Fields can no longer by added to layer %s",
                 m_poFeatureDefn->GetName());
        return OGRERR_FAILURE;
    }

    if (m_poVDV452Table != nullptr)
    {
        bool bFound = false;
        for (size_t i = 0; i < m_poVDV452Table->aosFields.size(); i++)
        {
            const char *pszFieldName = poFieldDefn->GetNameRef();
            if ((m_osVDV452Lang == "en" &&
                 EQUAL(m_poVDV452Table->aosFields[i].osEnglishName,
                       pszFieldName)) ||
                (m_osVDV452Lang == "de" &&
                 EQUAL(m_poVDV452Table->aosFields[i].osGermanName,
                       pszFieldName)))
            {
                bFound = true;
                break;
            }
        }
        if (!bFound)
        {
            CPLError(m_bProfileStrict ? CE_Failure : CE_Warning,
                     CPLE_AppDefined,
                     "Field %s is not an allowed field for table %s",
                     poFieldDefn->GetNameRef(), m_poFeatureDefn->GetName());
            if (m_bProfileStrict)
                return OGRERR_FAILURE;
        }
        if (EQUAL(m_poFeatureDefn->GetName(), "STOP") ||
            EQUAL(m_poFeatureDefn->GetName(), "REC_ORT"))
        {
            if (EQUAL(poFieldDefn->GetNameRef(), "POINT_LONGITUDE") ||
                EQUAL(poFieldDefn->GetNameRef(), "ORT_POS_LAENGE"))
            {
                m_iLongitudeVDV452 = m_poFeatureDefn->GetFieldCount();
            }
            else if (EQUAL(poFieldDefn->GetNameRef(), "POINT_LATITUDE") ||
                     EQUAL(poFieldDefn->GetNameRef(), "ORT_POS_BREITE"))
            {
                m_iLatitudeVDV452 = m_poFeatureDefn->GetFieldCount();
            }
        }
    }

    m_poFeatureDefn->AddFieldDefn(poFieldDefn);
    return OGRERR_NONE;
}

/************************************************************************/
/*                         TestCapability()                             */
/************************************************************************/

int OGRVDVWriterLayer::TestCapability(const char *pszCap)
{
    if (EQUAL(pszCap, OLCSequentialWrite))
        return m_bWritePossible;
    if (EQUAL(pszCap, OLCCreateField))
        return m_nFeatureCount < 0;
    return FALSE;
}

/************************************************************************/
/*                         StopAsCurrentLayer()                         */
/************************************************************************/

void OGRVDVWriterLayer::StopAsCurrentLayer()
{
    if (m_bWritePossible)
    {
        m_bWritePossible = false;
        if (m_fpL != nullptr)
        {
            WriteSchemaIfNeeded();
            VSIFPrintfL(m_fpL, "end; " CPL_FRMT_GIB "\n", m_nFeatureCount);
        }
    }
}

/************************************************************************/
/*                             GetDataset()                             */
/************************************************************************/

GDALDataset *OGRVDVWriterLayer::GetDataset()
{
    return m_poDS;
}

/************************************************************************/
/*                         OGRVDVWriteHeader()                          */
/************************************************************************/

static bool OGRVDVWriteHeader(VSILFILE *fpL, CSLConstList papszOptions)
{
    bool bRet = true;
    const bool bStandardHeader =
        CPLFetchBool(papszOptions, "STANDARD_HEADER", true);

    struct tm tm;
    CPLUnixTimeToYMDHMS(time(nullptr), &tm);
    const char *pszSrc = CSLFetchNameValueDef(
        papszOptions, "HEADER_SRC", (bStandardHeader) ? "UNKNOWN" : nullptr);
    const char *pszSrcDate = CSLFetchNameValueDef(
        papszOptions, "HEADER_SRC_DATE",
        (pszSrc) ? CPLSPrintf("%02d.%02d.%04d", tm.tm_mday, tm.tm_mon + 1,
                              tm.tm_year + 1900)
                 : nullptr);
    const char *pszSrcTime =
        CSLFetchNameValueDef(papszOptions, "HEADER_SRC_TIME",
                             (pszSrc) ? CPLSPrintf("%02d.%02d.%02d", tm.tm_hour,
                                                   tm.tm_min, tm.tm_sec)
                                      : nullptr);

    if (pszSrc && pszSrcDate && pszSrcTime)
    {
        bRet &= VSIFPrintfL(fpL, "mod; DD.MM.YYYY; HH:MM:SS; free\n") > 0;
        bRet &= VSIFPrintfL(fpL, "src; \"%s\"; \"%s\"; \"%s\"\n",
                            OGRVDVEscapeString(pszSrc).c_str(),
                            OGRVDVEscapeString(pszSrcDate).c_str(),
                            OGRVDVEscapeString(pszSrcTime).c_str()) > 0;
    }

    if (bStandardHeader)
    {
        const char *pszChs =
            CSLFetchNameValueDef(papszOptions, "HEADER_CHS", "ISO8859-1");
        const char *pszVer =
            CSLFetchNameValueDef(papszOptions, "HEADER_VER", "1.4");
        const char *pszIfv =
            CSLFetchNameValueDef(papszOptions, "HEADER_IFV", "1.4");
        const char *pszDve =
            CSLFetchNameValueDef(papszOptions, "HEADER_DVE", "1.4");
        const char *pszFft =
            CSLFetchNameValueDef(papszOptions, "HEADER_FFT", "");

        bRet &= VSIFPrintfL(fpL, "chs; \"%s\"\n",
                            OGRVDVEscapeString(pszChs).c_str()) > 0;
        bRet &= VSIFPrintfL(fpL, "ver; \"%s\"\n",
                            OGRVDVEscapeString(pszVer).c_str()) > 0;
        bRet &= VSIFPrintfL(fpL, "ifv; \"%s\"\n",
                            OGRVDVEscapeString(pszIfv).c_str()) > 0;
        bRet &= VSIFPrintfL(fpL, "dve; \"%s\"\n",
                            OGRVDVEscapeString(pszDve).c_str()) > 0;
        bRet &= VSIFPrintfL(fpL, "fft; \"%s\"\n",
                            OGRVDVEscapeString(pszFft).c_str()) > 0;
    }

    for (CSLConstList papszIter = papszOptions;
         papszIter != nullptr && *papszIter != nullptr; papszIter++)
    {
        if (STARTS_WITH_CI(*papszIter, "HEADER_") &&
            !STARTS_WITH_CI(*papszIter, "HEADER_SRC") &&
            (!bStandardHeader || (!EQUAL(*papszIter, "HEADER_CHS") &&
                                  !EQUAL(*papszIter, "HEADER_VER") &&
                                  !EQUAL(*papszIter, "HEADER_IFV") &&
                                  !EQUAL(*papszIter, "HEADER_DVE") &&
                                  !EQUAL(*papszIter, "HEADER_FFT"))))
        {
            char *pszKey = nullptr;
            const char *pszValue = CPLParseNameValue(*papszIter, &pszKey);
            if (pszKey && strlen(pszKey) > strlen("HEADER_") && pszValue)
            {
                bRet &=
                    VSIFPrintfL(fpL, "%s; \"%s\"\n", pszKey + strlen("HEADER_"),
                                OGRVDVEscapeString(pszValue).c_str()) > 0;
            }
            CPLFree(pszKey);
        }
    }

    return bRet;
}

/************************************************************************/
/*                      OGRVDVLoadVDV452Tables()                        */
/************************************************************************/

static bool OGRVDVLoadVDV452Tables(OGRVDV452Tables &oTables)
{
    CPLXMLNode *psRoot = nullptr;
#if defined(USE_ONLY_EMBEDDED_RESOURCE_FILES)
    const char *pszXMLDescFilename = nullptr;
#else
    const char *pszXMLDescFilename = CPLFindFile("gdal", "vdv452.xml");
#endif
    if (pszXMLDescFilename == nullptr ||
        EQUAL(pszXMLDescFilename, "vdv452.xml"))
    {
#ifdef EMBED_RESOURCE_FILES
        static const bool bOnce [[maybe_unused]] = []()
        {
            CPLDebug("VDV", "Using embedded vdv452.xml");
            return true;
        }();
        psRoot = CPLParseXMLString(VDVGet452XML());
#else
        CPLDebug("VDV", "Cannot find XML file : %s", "vdv452.xml");
        return false;
#endif
    }

#ifdef EMBED_RESOURCE_FILES
    if (!psRoot)
#endif
    {
        psRoot = CPLParseXMLFile(pszXMLDescFilename);
    }
    if (psRoot == nullptr)
    {
        return false;
    }
    CPLXMLNode *psTables = CPLGetXMLNode(psRoot, "=Layers");
    if (psTables != nullptr)
    {
        for (CPLXMLNode *psTable = psTables->psChild; psTable != nullptr;
             psTable = psTable->psNext)
        {
            if (psTable->eType != CXT_Element ||
                strcmp(psTable->pszValue, "Layer") != 0)
                continue;
            OGRVDV452Table *poTable = new OGRVDV452Table();
            poTable->osEnglishName = CPLGetXMLValue(psTable, "name_en", "");
            poTable->osGermanName = CPLGetXMLValue(psTable, "name_de", "");
            oTables.aosTables.push_back(poTable);
            oTables.oMapEnglish[poTable->osEnglishName] = poTable;
            oTables.oMapGerman[poTable->osGermanName] = poTable;
            for (CPLXMLNode *psField = psTable->psChild; psField != nullptr;
                 psField = psField->psNext)
            {
                if (psField->eType != CXT_Element ||
                    strcmp(psField->pszValue, "Field") != 0)
                    continue;
                OGRVDV452Field oField;
                oField.osEnglishName = CPLGetXMLValue(psField, "name_en", "");
                oField.osGermanName = CPLGetXMLValue(psField, "name_de", "");
                oField.osType = CPLGetXMLValue(psField, "type", "");
                oField.nWidth = atoi(CPLGetXMLValue(psField, "width", "0"));
                poTable->aosFields.push_back(oField);
            }
        }
    }

    CPLDestroyXMLNode(psRoot);
    return true;
}

/************************************************************************/
/*                           ICreateLayer()                             */
/************************************************************************/

OGRLayer *
OGRVDVDataSource::ICreateLayer(const char *pszLayerName,
                               const OGRGeomFieldDefn *poGeomFieldDefn,
                               CSLConstList papszOptions)
{
    if (!m_bUpdate)
        return nullptr;

    const char *pszProfile =
        CSLFetchNameValueDef(papszOptions, "PROFILE", "GENERIC");
    if (STARTS_WITH_CI(pszProfile, "VDV-452") && !m_bVDV452Loaded)
    {
        m_bVDV452Loaded = true;
        OGRVDVLoadVDV452Tables(m_oVDV452Tables);
    }
    const bool bProfileStrict =
        CPLFetchBool(papszOptions, "PROFILE_STRICT", false);
    const bool bCreateAllFields =
        CPLFetchBool(papszOptions, "CREATE_ALL_FIELDS", true);

    CPLString osUpperLayerName(pszLayerName);
    osUpperLayerName.toupper();

    OGRVDV452Table *poVDV452Table = nullptr;
    CPLString osVDV452Lang;
    bool bOKTable = true;
    if (EQUAL(pszProfile, "VDV-452"))
    {
        if (m_oVDV452Tables.oMapEnglish.find(osUpperLayerName) !=
            m_oVDV452Tables.oMapEnglish.end())
        {
            poVDV452Table = m_oVDV452Tables.oMapEnglish[osUpperLayerName];
            osVDV452Lang = "en";
        }
        else if (m_oVDV452Tables.oMapGerman.find(osUpperLayerName) !=
                 m_oVDV452Tables.oMapGerman.end())
        {
            poVDV452Table = m_oVDV452Tables.oMapGerman[osUpperLayerName];
            osVDV452Lang = "de";
        }
        else
        {
            bOKTable = false;
        }
    }
    else if (EQUAL(pszProfile, "VDV-452-ENGLISH"))
    {
        if (m_oVDV452Tables.oMapEnglish.find(osUpperLayerName) !=
            m_oVDV452Tables.oMapEnglish.end())
        {
            poVDV452Table = m_oVDV452Tables.oMapEnglish[osUpperLayerName];
            osVDV452Lang = "en";
        }
        else
        {
            bOKTable = false;
        }
    }
    else if (EQUAL(pszProfile, "VDV-452-GERMAN"))
    {
        if (m_oVDV452Tables.oMapGerman.find(osUpperLayerName) !=
            m_oVDV452Tables.oMapGerman.end())
        {
            poVDV452Table = m_oVDV452Tables.oMapGerman[osUpperLayerName];
            osVDV452Lang = "de";
        }
        else
        {
            bOKTable = false;
        }
    }
    if (!bOKTable)
    {
        CPLError(bProfileStrict ? CE_Failure : CE_Warning, CPLE_AppDefined,
                 "%s is not a VDV-452 table", pszLayerName);
        if (bProfileStrict)
            return nullptr;
    }

    VSILFILE *fpL = nullptr;
    if (m_bSingleFile)
    {
        fpL = m_fpL;
        if (!m_bNew && m_nLayerCount == 0)
        {
            // Find last non-empty line in the file
            VSIFSeekL(fpL, 0, SEEK_END);
            vsi_l_offset nFileSize = VSIFTellL(fpL);
            vsi_l_offset nOffset = nFileSize;
            bool bTerminatingEOL = true;
            while (nOffset > 0)
            {
                VSIFSeekL(fpL, nOffset - 1, SEEK_SET);
                char ch = '\0';
                VSIFReadL(&ch, 1, 1, fpL);
                if (bTerminatingEOL)
                {
                    if (!(ch == '\r' || ch == '\n'))
                    {
                        bTerminatingEOL = false;
                    }
                }
                else
                {
                    if (ch == '\r' || ch == '\n')
                        break;
                }
                nOffset--;
            }

            // If it is "eof;..." then overwrite it with new content
            const char *pszLine = CPLReadLineL(fpL);
            if (pszLine != nullptr && STARTS_WITH(pszLine, "eof;"))
            {
                VSIFSeekL(fpL, nOffset, SEEK_SET);
                VSIFTruncateL(fpL, VSIFTellL(fpL));
            }
            else if (nFileSize > 0)
            {
                // Otherwise make sure the file ends with an eol character
                VSIFSeekL(fpL, nFileSize - 1, SEEK_SET);
                char ch = '\0';
                VSIFReadL(&ch, 1, 1, fpL);
                VSIFSeekL(fpL, nFileSize, SEEK_SET);
                if (!(ch == '\r' || ch == '\n'))
                {
                    ch = '\n';
                    VSIFWriteL(&ch, 1, 1, fpL);
                }
            }
        }
    }
    else
    {
        CPLString osExtension =
            CSLFetchNameValueDef(papszOptions, "EXTENSION", "x10");
        const CPLString osFilename =
            CPLFormFilenameSafe(m_osFilename, pszLayerName, osExtension);
        fpL = VSIFOpenL(osFilename, "wb");
        if (fpL == nullptr)
        {
            CPLError(CE_Failure, CPLE_FileIO, "Cannot create %s",
                     osFilename.c_str());
            return nullptr;
        }
    }

    GetLayerCount();

    if (m_nLayerCount == 0 || !m_bSingleFile)
    {
        if (!OGRVDVWriteHeader(fpL, papszOptions))
        {
            if (!m_bSingleFile)
                VSIFCloseL(fpL);
            return nullptr;
        }
    }

    m_bMustWriteEof = true;

    OGRVDVWriterLayer *poLayer =
        new OGRVDVWriterLayer(this, pszLayerName, fpL, !m_bSingleFile,
                              poVDV452Table, osVDV452Lang, bProfileStrict);
    m_papoLayers = static_cast<OGRLayer **>(
        CPLRealloc(m_papoLayers, sizeof(OGRLayer *) * (m_nLayerCount + 1)));
    m_papoLayers[m_nLayerCount] = poLayer;
    m_nLayerCount++;

    const auto eGType = poGeomFieldDefn ? poGeomFieldDefn->GetType() : wkbNone;
    if (eGType == wkbPoint && poVDV452Table != nullptr &&
        (EQUAL(pszLayerName, "STOP") || EQUAL(pszLayerName, "REC_ORT")))
    {
        poLayer->GetLayerDefn()->SetGeomType(wkbPoint);
    }

    if (bCreateAllFields && poVDV452Table != nullptr)
    {
        for (size_t i = 0; i < poVDV452Table->aosFields.size(); i++)
        {
            const char *pszFieldName =
                (osVDV452Lang == "en")
                    ? poVDV452Table->aosFields[i].osEnglishName.c_str()
                    : poVDV452Table->aosFields[i].osGermanName.c_str();
            OGRFieldType eType = OFTString;
            int nWidth = poVDV452Table->aosFields[i].nWidth;
            if (poVDV452Table->aosFields[i].osType == "num" ||
                poVDV452Table->aosFields[i].osType == "boolean")
                eType = OFTInteger;
            if (poVDV452Table->aosFields[i].osType == "num")
            {
                /* VDV 451 is without sign */
                nWidth++;
                if (nWidth >= 10)
                    eType = OFTInteger64;
            }
            OGRFieldDefn oField(pszFieldName, eType);
            if (poVDV452Table->aosFields[i].osType == "boolean")
                oField.SetSubType(OFSTBoolean);
            oField.SetWidth(nWidth);
            poLayer->CreateField(&oField);
        }
    }

    return poLayer;
}

/************************************************************************/
/*                       SetCurrentWriterLayer()                        */
/************************************************************************/

void OGRVDVDataSource::SetCurrentWriterLayer(OGRVDVWriterLayer *poLayer)
{
    if (!m_bSingleFile)
        return;
    if (m_poCurrentWriterLayer != nullptr && m_poCurrentWriterLayer != poLayer)
    {
        m_poCurrentWriterLayer->StopAsCurrentLayer();
    }
    m_poCurrentWriterLayer = poLayer;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRVDVDataSource::TestCapability(const char *pszCap)

{
    if (EQUAL(pszCap, ODsCCreateLayer))
        return m_bUpdate;
    else if (EQUAL(pszCap, ODsCZGeometries))
        return true;

    return false;
}

/************************************************************************/
/*                                 Create()                             */
/************************************************************************/

GDALDataset *OGRVDVDataSource::Create(const char *pszName, int /*nXSize*/,
                                      int /*nYSize*/, int /*nBands*/,
                                      GDALDataType /*eType*/,
                                      char **papszOptions)

{
    /* -------------------------------------------------------------------- */
    /*      First, ensure there isn't any such file yet.                    */
    /* -------------------------------------------------------------------- */
    VSIStatBufL sStatBuf;
    if (VSIStatL(pszName, &sStatBuf) == 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "It seems a file system object called '%s' already exists.",
                 pszName);

        return nullptr;
    }

    const bool bSingleFile = CPLFetchBool(papszOptions, "SINGLE_FILE", true);
    if (!bSingleFile)
    {
        if (VSIMkdir(pszName, 0755) != 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Failed to create directory %s:\n%s", pszName,
                     VSIStrerror(errno));
            return nullptr;
        }
    }

    VSILFILE *fpL = nullptr;
    if (bSingleFile)
    {
        fpL = VSIFOpenL(pszName, "wb");
        if (fpL == nullptr)
        {
            CPLError(CE_Failure, CPLE_FileIO, "Cannot create %s", pszName);
            return nullptr;
        }
    }
    OGRVDVDataSource *poDS =
        new OGRVDVDataSource(pszName, fpL, true, bSingleFile, true /* new */);
    return poDS;
}

/************************************************************************/
/*                         RegisterOGRVDV()                             */
/************************************************************************/

void RegisterOGRVDV()

{
    if (GDALGetDriverByName("VDV") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("VDV");
    poDriver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE_LAYER, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE_FIELD, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_DELETE_FIELD, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_REORDER_FIELDS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_MEASURED_GEOMETRIES, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CURVE_GEOMETRIES, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_Z_GEOMETRIES, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_CREATION_FIELD_DEFN_FLAGS,
                              "WidthPrecision");
    poDriver->SetMetadataItem(GDAL_DMD_ALTER_FIELD_DEFN_FLAGS,
                              "Name Type WidthPrecision");
    poDriver->SetMetadataItem(GDAL_DMD_SUPPORTED_SQL_DIALECTS, "OGRSQL SQLITE");

    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME,
                              "VDV-451/VDV-452/INTREST Data Format");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/vector/vdv.html");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSIONS, "txt x10");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_CREATIONFIELDDATATYPES,
                              "Integer Integer64 String");

    poDriver->SetMetadataItem(
        GDAL_DMD_CREATIONOPTIONLIST,
        "<CreationOptionList>"
        "  <Option name='SINGLE_FILE' type='boolean' description='Whether "
        "several layers "
        "should be put in the same file. If no, the name is assumed to be a "
        "directory name' default='YES'/>"
        "</CreationOptionList>");

    poDriver->SetMetadataItem(
        GDAL_DS_LAYER_CREATIONOPTIONLIST,
        "<LayerCreationOptionList>"
        "  <Option name='EXTENSION' type='string' description='Layer file "
        "extension. Only used for SINGLE_FILE=NO' default='x10'/>"
        "  <Option name='PROFILE' type='string-select' description='Profile' "
        "default='GENERIC'>"
        "       <Value>GENERIC</Value>"
        "       <Value>VDV-452</Value>"
        "       <Value>VDV-452-ENGLISH</Value>"
        "       <Value>VDV-452-GERMAN</Value>"
        "  </Option>"
        "  <Option name='PROFILE_STRICT' type='boolean' description='Whether "
        "checks of profile should be strict' default='NO'/>"
        "  <Option name='CREATE_ALL_FIELDS' type='boolean' description="
        "'Whether all fields of predefined profiles should be created at layer "
        "creation' default='YES'/>"
        "  <Option name='STANDARD_HEADER' type='boolean' description='Whether "
        "to write standard header fields' default='YES'/>"
        "  <Option name='HEADER_SRC' type='string' description='Value of the "
        "src header field' default='UNKNOWN'/>"
        "  <Option name='HEADER_SRC_DATE' type='string' description='Value of "
        "the date of the src header field as DD.MM.YYYY'/>"
        "  <Option name='HEADER_SRC_TIME' type='string' description='Value of "
        "the time of the src header field as HH.MM.SS'/>"
        "  <Option name='HEADER_CHS' type='string' description='Value of the "
        "chs header field' default='ISO8859-1'/>"
        "  <Option name='HEADER_VER' type='string' description='Value of the "
        "ver header field' default='1.4'/>"
        "  <Option name='HEADER_IFV' type='string' description='Value of the "
        "ifv header field' default='1.4'/>"
        "  <Option name='HEADER_DVE' type='string' description='Value of the "
        "dve header field' default='1.4'/>"
        "  <Option name='HEADER_FFT' type='string' description='Value of the "
        "fft header field' default=''/>"
        "  <Option name='HEADER_*' type='string' description='Value of another "
        "header field'/>"
        "</LayerCreationOptionList>");
    poDriver->pfnIdentify = OGRVDVDriverIdentify;
    poDriver->pfnOpen = OGRVDVDataSource::Open;
    poDriver->pfnCreate = OGRVDVDataSource::Create;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
