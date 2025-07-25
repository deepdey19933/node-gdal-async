/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements Open FileGDB OGR driver.
 * Author:   Even Rouault, <even dot rouault at spatialys.com>
 *
 ******************************************************************************
 * Copyright (c) 2021, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef FILEGDB_FIELDDOMAIN_H
#define FILEGDB_FIELDDOMAIN_H

#include "cpl_minixml.h"
#include "filegdb_gdbtoogrfieldtype.h"
#include "ogr_p.h"

/************************************************************************/
/*                      ParseXMLFieldDomainDef()                        */
/************************************************************************/

inline std::unique_ptr<OGRFieldDomain>
ParseXMLFieldDomainDef(const std::string &domainDef)
{
    CPLXMLTreeCloser oTree(CPLParseXMLString(domainDef.c_str()));
    if (!oTree.get())
    {
        return nullptr;
    }
    const CPLXMLNode *psDomain = CPLGetXMLNode(oTree.get(), "=esri:Domain");
    if (psDomain == nullptr)
    {
        // esri: namespace prefix omitted when called from the FileGDB driver
        psDomain = CPLGetXMLNode(oTree.get(), "=Domain");
    }
    bool bIsCodedValueDomain = false;
    if (psDomain == nullptr)
    {
        // Also sometimes found...
        psDomain = CPLGetXMLNode(oTree.get(), "=esri:CodedValueDomain");
        if (psDomain)
            bIsCodedValueDomain = true;
    }
    if (psDomain == nullptr)
    {
        // Also sometimes found...
        psDomain = CPLGetXMLNode(oTree.get(), "=typens:GPCodedValueDomain2");
        if (psDomain)
            bIsCodedValueDomain = true;
    }
    if (psDomain == nullptr)
    {
        // Also sometimes found...
        psDomain = CPLGetXMLNode(oTree.get(), "=GPCodedValueDomain2");
        if (psDomain)
            bIsCodedValueDomain = true;
    }
    bool bIsRangeDomain = false;
    if (psDomain == nullptr)
    {
        // Also sometimes found...
        psDomain = CPLGetXMLNode(oTree.get(), "=esri:RangeDomain");
        if (psDomain)
            bIsRangeDomain = true;
    }
    if (psDomain == nullptr)
    {
        // Also sometimes found...
        psDomain = CPLGetXMLNode(oTree.get(), "=typens:GPRangeDomain2");
        if (psDomain)
            bIsRangeDomain = true;
    }
    if (psDomain == nullptr)
    {
        // Also sometimes found...
        psDomain = CPLGetXMLNode(oTree.get(), "=GPRangeDomain2");
        if (psDomain)
            bIsRangeDomain = true;
    }
    if (psDomain == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Cannot find root 'Domain' node");
        return nullptr;
    }
    const char *pszType = CPLGetXMLValue(psDomain, "xsi:type", "");
    const char *pszName = CPLGetXMLValue(psDomain, "DomainName", "");
    const char *pszDescription = CPLGetXMLValue(psDomain, "Description", "");
    const char *pszFieldType = CPLGetXMLValue(psDomain, "FieldType", "");
    OGRFieldType eFieldType = OFTString;
    OGRFieldSubType eSubType = OFSTNone;
    if (!GDBToOGRFieldType(pszFieldType, &eFieldType, &eSubType))
    {
        return nullptr;
    }

    std::unique_ptr<OGRFieldDomain> domain;
    if (bIsCodedValueDomain || strcmp(pszType, "esri:CodedValueDomain") == 0)
    {
        const CPLXMLNode *psCodedValues =
            CPLGetXMLNode(psDomain, "CodedValues");
        if (psCodedValues == nullptr)
        {
            return nullptr;
        }
        std::vector<OGRCodedValue> asValues;
        for (const CPLXMLNode *psIter = psCodedValues->psChild; psIter;
             psIter = psIter->psNext)
        {
            if (psIter->eType == CXT_Element &&
                strcmp(psIter->pszValue, "CodedValue") == 0)
            {
                OGRCodedValue cv;
                cv.pszCode = CPLStrdup(CPLGetXMLValue(psIter, "Code", ""));
                cv.pszValue = CPLStrdup(CPLGetXMLValue(psIter, "Name", ""));
                asValues.emplace_back(cv);
            }
        }
        domain.reset(new OGRCodedFieldDomain(pszName, pszDescription,
                                             eFieldType, eSubType,
                                             std::move(asValues)));
    }
    else if (bIsRangeDomain || strcmp(pszType, "esri:RangeDomain") == 0)
    {
        if (eFieldType != OFTInteger && eFieldType != OFTInteger64 &&
            eFieldType != OFTReal && eFieldType != OFTDateTime)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Unsupported field type for range domain: %s",
                     pszFieldType);
            return nullptr;
        }
        const char *pszMinValue = CPLGetXMLValue(psDomain, "MinValue", "");
        const char *pszMaxValue = CPLGetXMLValue(psDomain, "MaxValue", "");
        OGRField sMin;
        OGRField sMax;
        OGR_RawField_SetUnset(&sMin);
        OGR_RawField_SetUnset(&sMax);
        if (eFieldType == OFTInteger)
        {
            sMin.Integer = atoi(pszMinValue);
            sMax.Integer = atoi(pszMaxValue);
        }
        else if (eFieldType == OFTInteger64)
        {
            sMin.Integer64 = CPLAtoGIntBig(pszMinValue);
            sMax.Integer64 = CPLAtoGIntBig(pszMaxValue);
        }
        else if (eFieldType == OFTReal)
        {
            sMin.Real = CPLAtof(pszMinValue);
            sMax.Real = CPLAtof(pszMaxValue);
        }
        else if (eFieldType == OFTDateTime)
        {
            if (!OGRParseXMLDateTime(pszMinValue, &sMin))
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Invalid MinValue: %s",
                         pszMinValue);
                return nullptr;
            }
            if (!OGRParseXMLDateTime(pszMaxValue, &sMax))
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Invalid MaxValue: %s",
                         pszMaxValue);
                return nullptr;
            }
        }
        domain.reset(new OGRRangeFieldDomain(pszName, pszDescription,
                                             eFieldType, eSubType, sMin, true,
                                             sMax, true));
    }
    else
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Unsupported type of File Geodatabase domain: %s", pszType);
        return nullptr;
    }

    const char *pszMergePolicy =
        CPLGetXMLValue(psDomain, "MergePolicy", "esriMPTDefaultValue");
    if (EQUAL(pszMergePolicy, "esriMPTDefaultValue"))
    {
        domain->SetMergePolicy(OFDMP_DEFAULT_VALUE);
    }
    else if (EQUAL(pszMergePolicy, "esriMPTSumValues"))
    {
        domain->SetMergePolicy(OFDMP_SUM);
    }
    else if (EQUAL(pszMergePolicy, "esriMPTAreaWeighted"))
    {
        domain->SetMergePolicy(OFDMP_GEOMETRY_WEIGHTED);
    }

    const char *pszSplitPolicy =
        CPLGetXMLValue(psDomain, "SplitPolicy", "esriSPTDefaultValue");
    if (EQUAL(pszSplitPolicy, "esriSPTDefaultValue"))
    {
        domain->SetSplitPolicy(OFDSP_DEFAULT_VALUE);
    }
    else if (EQUAL(pszSplitPolicy, "esriSPTDuplicate"))
    {
        domain->SetSplitPolicy(OFDSP_DUPLICATE);
    }
    else if (EQUAL(pszSplitPolicy, "esriSPTGeometryRatio"))
    {
        domain->SetSplitPolicy(OFDSP_GEOMETRY_RATIO);
    }

    return domain;
}

/************************************************************************/
/*                      BuildXMLFieldDomainDef()                        */
/************************************************************************/

inline std::string BuildXMLFieldDomainDef(const OGRFieldDomain *poDomain,
                                          bool bForFileGDBSDK,
                                          std::string &failureReason)
{
    std::string osNS = "esri";
    const char *pszRootElt = "esri:Domain";
    if (!bForFileGDBSDK)
    {
        switch (poDomain->GetDomainType())
        {
            case OFDT_CODED:
            {
                pszRootElt = "typens:GPCodedValueDomain2";
                break;
            }

            case OFDT_RANGE:
            {
                pszRootElt = "typens:GPRangeDomain2";
                break;
            }

            case OFDT_GLOB:
            {
                failureReason =
                    "Glob field domain not handled for FileGeoDatabase";
                return std::string();
            }
        }
        osNS = "typens";
    }

    CPLXMLTreeCloser oTree(CPLCreateXMLNode(nullptr, CXT_Element, pszRootElt));
    CPLXMLNode *psRoot = oTree.get();

    switch (poDomain->GetDomainType())
    {
        case OFDT_CODED:
        {
            CPLAddXMLAttributeAndValue(psRoot, "xsi:type",
                                       bForFileGDBSDK
                                           ? "esri:CodedValueDomain"
                                           : "typens:GPCodedValueDomain2");
            break;
        }

        case OFDT_RANGE:
        {
            CPLAddXMLAttributeAndValue(
                psRoot, "xsi:type",
                bForFileGDBSDK ? "esri:RangeDomain" : "typens:GPRangeDomain2");
            break;
        }

        case OFDT_GLOB:
        {
            failureReason = "Glob field domain not handled for FileGeoDatabase";
            return std::string();
        }
    }

    CPLAddXMLAttributeAndValue(psRoot, "xmlns:xsi",
                               "http://www.w3.org/2001/XMLSchema-instance");
    CPLAddXMLAttributeAndValue(psRoot, "xmlns:xs",
                               "http://www.w3.org/2001/XMLSchema");
    CPLAddXMLAttributeAndValue(psRoot, ("xmlns:" + osNS).c_str(),
                               "http://www.esri.com/schemas/ArcGIS/10.1");

    CPLCreateXMLElementAndValue(psRoot, "DomainName",
                                poDomain->GetName().c_str());
    if (poDomain->GetFieldType() == OFTInteger)
    {
        if (poDomain->GetFieldSubType() == OFSTInt16)
            CPLCreateXMLElementAndValue(psRoot, "FieldType",
                                        "esriFieldTypeSmallInteger");
        else
            CPLCreateXMLElementAndValue(psRoot, "FieldType",
                                        "esriFieldTypeInteger");
    }
    else if (poDomain->GetFieldType() == OFTReal)
    {
        if (poDomain->GetFieldSubType() == OFSTFloat32)
            CPLCreateXMLElementAndValue(psRoot, "FieldType",
                                        "esriFieldTypeSingle");
        else
            CPLCreateXMLElementAndValue(psRoot, "FieldType",
                                        "esriFieldTypeDouble");
    }
    else if (poDomain->GetFieldType() == OFTString)
    {
        CPLCreateXMLElementAndValue(psRoot, "FieldType", "esriFieldTypeString");
    }
    else if (poDomain->GetFieldType() == OFTDateTime)
    {
        CPLCreateXMLElementAndValue(psRoot, "FieldType", "esriFieldTypeDate");
    }
    else
    {
        failureReason = "Unsupported field type for FileGeoDatabase domain";
        return std::string();
    }

    switch (poDomain->GetMergePolicy())
    {
        case OFDMP_DEFAULT_VALUE:
            CPLCreateXMLElementAndValue(psRoot, "MergePolicy",
                                        "esriMPTDefaultValue");
            break;
        case OFDMP_SUM:
            CPLCreateXMLElementAndValue(psRoot, "MergePolicy",
                                        "esriMPTSumValues");
            break;
        case OFDMP_GEOMETRY_WEIGHTED:
            CPLCreateXMLElementAndValue(psRoot, "MergePolicy",
                                        "esriMPTAreaWeighted");
            break;
    }

    switch (poDomain->GetSplitPolicy())
    {
        case OFDSP_DEFAULT_VALUE:
            CPLCreateXMLElementAndValue(psRoot, "SplitPolicy",
                                        "esriSPTDefaultValue");
            break;
        case OFDSP_DUPLICATE:
            CPLCreateXMLElementAndValue(psRoot, "SplitPolicy",
                                        "esriSPTDuplicate");
            break;
        case OFDSP_GEOMETRY_RATIO:
            CPLCreateXMLElementAndValue(psRoot, "SplitPolicy",
                                        "esriSPTGeometryRatio");
            break;
    }

    CPLCreateXMLElementAndValue(psRoot, "Description",
                                poDomain->GetDescription().c_str());
    CPLCreateXMLElementAndValue(psRoot, "Owner", "");

    const auto AddFieldTypeAsXSIType = [&poDomain](CPLXMLNode *psParent)
    {
        if (poDomain->GetFieldType() == OFTInteger)
        {
            if (poDomain->GetFieldSubType() == OFSTInt16)
                CPLAddXMLAttributeAndValue(psParent, "xsi:type", "xs:short");
            else
                CPLAddXMLAttributeAndValue(psParent, "xsi:type", "xs:int");
        }
        else if (poDomain->GetFieldType() == OFTReal)
        {
            if (poDomain->GetFieldSubType() == OFSTFloat32)
                CPLAddXMLAttributeAndValue(psParent, "xsi:type", "xs:float");
            else
                CPLAddXMLAttributeAndValue(psParent, "xsi:type", "xs:double");
        }
        else if (poDomain->GetFieldType() == OFTString)
        {
            CPLAddXMLAttributeAndValue(psParent, "xsi:type", "xs:string");
        }
        else if (poDomain->GetFieldType() == OFTDateTime)
        {
            CPLAddXMLAttributeAndValue(psParent, "xsi:type", "xs:dateTime");
        }
    };

    switch (poDomain->GetDomainType())
    {
        case OFDT_CODED:
        {
            auto psCodedValues =
                CPLCreateXMLNode(psRoot, CXT_Element, "CodedValues");
            CPLAddXMLAttributeAndValue(psCodedValues, "xsi:type",
                                       (osNS + ":ArrayOfCodedValue").c_str());

            auto poCodedDomain =
                cpl::down_cast<const OGRCodedFieldDomain *>(poDomain);
            const OGRCodedValue *psEnumeration =
                poCodedDomain->GetEnumeration();
            for (; psEnumeration->pszCode != nullptr; ++psEnumeration)
            {
                auto psCodedValue =
                    CPLCreateXMLNode(psCodedValues, CXT_Element, "CodedValue");
                CPLAddXMLAttributeAndValue(psCodedValue, "xsi:type",
                                           (osNS + ":CodedValue").c_str());
                CPLCreateXMLElementAndValue(
                    psCodedValue, "Name",
                    psEnumeration->pszValue ? psEnumeration->pszValue : "");

                auto psCode =
                    CPLCreateXMLNode(psCodedValue, CXT_Element, "Code");
                AddFieldTypeAsXSIType(psCode);
                CPLCreateXMLNode(psCode, CXT_Text, psEnumeration->pszCode);
            }
            break;
        }

        case OFDT_RANGE:
        {
            auto poRangeDomain =
                cpl::down_cast<const OGRRangeFieldDomain *>(poDomain);

            const auto SerializeMinOrMax =
                [&AddFieldTypeAsXSIType, &poDomain,
                 psRoot](const char *pszElementName, const OGRField &oValue)
            {
                auto psValue =
                    CPLCreateXMLNode(psRoot, CXT_Element, pszElementName);
                AddFieldTypeAsXSIType(psValue);
                if (poDomain->GetFieldType() == OFTInteger)
                {
                    CPLCreateXMLNode(psValue, CXT_Text,
                                     CPLSPrintf("%d", oValue.Integer));
                }
                else if (poDomain->GetFieldType() == OFTReal)
                {
                    CPLCreateXMLNode(psValue, CXT_Text,
                                     CPLSPrintf("%.18g", oValue.Real));
                }
                else if (poDomain->GetFieldType() == OFTDateTime)
                {
                    CPLCreateXMLNode(
                        psValue, CXT_Text,
                        CPLSPrintf("%04d-%02d-%02dT%02d:%02d:%02d",
                                   oValue.Date.Year, oValue.Date.Month,
                                   oValue.Date.Day, oValue.Date.Hour,
                                   oValue.Date.Minute,
                                   static_cast<int>(oValue.Date.Second + 0.5)));
                }
            };

            bool bIsInclusiveOut = false;
            const OGRField &oMin = poRangeDomain->GetMin(bIsInclusiveOut);
            const OGRField &oMax = poRangeDomain->GetMax(bIsInclusiveOut);
            if (OGR_RawField_IsUnset(&oMin) || OGR_RawField_IsUnset(&oMax))
            {
                failureReason =
                    "FileGeoDatabase requires that both minimum and maximum "
                    "values of a range field domain are set.";
                return std::string();
            }

            SerializeMinOrMax("MaxValue", oMax);
            SerializeMinOrMax("MinValue", oMin);

            break;
        }

        case OFDT_GLOB:
        {
            CPLAssert(false);
            break;
        }
    }

    char *pszXML = CPLSerializeXMLTree(oTree.get());
    const std::string osXML(pszXML);
    CPLFree(pszXML);
    return osXML;
}

#endif  // FILEGDB_FIELDDOMAIN_H
