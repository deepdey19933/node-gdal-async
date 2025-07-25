/******************************************************************************

 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements OGRPGTableLayer class, access to an existing table.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2000, Frank Warmerdam
 * Copyright (c) 2008-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_pg.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "cpl_error.h"
#include "ogr_p.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#define PQexec this_is_an_error

#define UNSUPPORTED_OP_READ_ONLY                                               \
    "%s : unsupported operation on a read-only datasource."

/************************************************************************/
/*                        OGRPGTableFeatureDefn                         */
/************************************************************************/

class OGRPGTableFeatureDefn final : public OGRPGFeatureDefn
{
  private:
    OGRPGTableFeatureDefn(const OGRPGTableFeatureDefn &) = delete;
    OGRPGTableFeatureDefn &operator=(const OGRPGTableFeatureDefn &) = delete;

    OGRPGTableLayer *poLayer = nullptr;

    void SolveFields() const;

  public:
    explicit OGRPGTableFeatureDefn(OGRPGTableLayer *poLayerIn,
                                   const char *pszName = nullptr)
        : OGRPGFeatureDefn(pszName), poLayer(poLayerIn)
    {
    }

    virtual void UnsetLayer() override
    {
        poLayer = nullptr;
        OGRPGFeatureDefn::UnsetLayer();
    }

    virtual int GetFieldCount() const override
    {
        SolveFields();
        return OGRPGFeatureDefn::GetFieldCount();
    }

    virtual OGRFieldDefn *GetFieldDefn(int i) override
    {
        SolveFields();
        return OGRPGFeatureDefn::GetFieldDefn(i);
    }

    virtual const OGRFieldDefn *GetFieldDefn(int i) const override
    {
        SolveFields();
        return OGRPGFeatureDefn::GetFieldDefn(i);
    }

    virtual int GetFieldIndex(const char *pszName) const override
    {
        SolveFields();
        return OGRPGFeatureDefn::GetFieldIndex(pszName);
    }

    virtual int GetGeomFieldCount() const override
    {
        if (poLayer != nullptr && !poLayer->HasGeometryInformation())
            SolveFields();
        return OGRPGFeatureDefn::GetGeomFieldCount();
    }

    virtual OGRPGGeomFieldDefn *GetGeomFieldDefn(int i) override
    {
        if (poLayer != nullptr && !poLayer->HasGeometryInformation())
            SolveFields();
        return OGRPGFeatureDefn::GetGeomFieldDefn(i);
    }

    virtual const OGRPGGeomFieldDefn *GetGeomFieldDefn(int i) const override
    {
        if (poLayer != nullptr && !poLayer->HasGeometryInformation())
            SolveFields();
        return OGRPGFeatureDefn::GetGeomFieldDefn(i);
    }

    virtual int GetGeomFieldIndex(const char *pszName) const override
    {
        if (poLayer != nullptr && !poLayer->HasGeometryInformation())
            SolveFields();
        return OGRPGFeatureDefn::GetGeomFieldIndex(pszName);
    }
};

/************************************************************************/
/*                           SolveFields()                              */
/************************************************************************/

void OGRPGTableFeatureDefn::SolveFields() const
{
    if (poLayer == nullptr)
        return;

    poLayer->ReadTableDefinition();
}

/************************************************************************/
/*                            GetFIDColumn()                            */
/************************************************************************/

const char *OGRPGTableLayer::GetFIDColumn()

{
    ReadTableDefinition();

    if (pszFIDColumn != nullptr)
        return pszFIDColumn;
    else
        return "";
}

/************************************************************************/
/*                          OGRPGTableLayer()                           */
/************************************************************************/

OGRPGTableLayer::OGRPGTableLayer(OGRPGDataSource *poDSIn,
                                 CPLString &osCurrentSchema,
                                 const char *pszTableNameIn,
                                 const char *pszSchemaNameIn,
                                 const char *pszDescriptionIn,
                                 const char *pszGeomColForcedIn, int bUpdate)
    : bUpdateAccess(bUpdate), pszTableName(CPLStrdup(pszTableNameIn)),
      pszSchemaName(CPLStrdup(pszSchemaNameIn ? pszSchemaNameIn
                                              : osCurrentSchema.c_str())),
      m_pszTableDescription(pszDescriptionIn ? CPLStrdup(pszDescriptionIn)
                                             : nullptr),
      osPrimaryKey(CPLGetConfigOption("PGSQL_OGR_FID", "ogc_fid")),
      pszGeomColForced(pszGeomColForcedIn ? CPLStrdup(pszGeomColForcedIn)
                                          : nullptr),
      // Just in provision for people yelling about broken backward
      // compatibility.
      bRetrieveFID(
          CPLTestBool(CPLGetConfigOption("OGR_PG_RETRIEVE_FID", "TRUE"))),
      bSkipConflicts(
          CPLTestBool(CPLGetConfigOption("OGR_PG_SKIP_CONFLICTS", "FALSE")))
{
    poDS = poDSIn;
    pszQueryStatement = nullptr;

    /* -------------------------------------------------------------------- */
    /*      Build the layer defn name.                                      */
    /* -------------------------------------------------------------------- */
    CPLString osDefnName;
    if (pszSchemaNameIn && osCurrentSchema != pszSchemaNameIn)
    {
        osDefnName.Printf("%s.%s", pszSchemaNameIn, pszTableName);
        pszSqlTableName = CPLStrdup(CPLString().Printf(
            "%s.%s", OGRPGEscapeColumnName(pszSchemaNameIn).c_str(),
            OGRPGEscapeColumnName(pszTableName).c_str()));
    }
    else
    {
        // no prefix for current_schema in layer name, for backwards
        // compatibility.
        osDefnName = pszTableName;
        pszSqlTableName = CPLStrdup(OGRPGEscapeColumnName(pszTableName));
    }
    if (pszGeomColForced != nullptr)
    {
        osDefnName += "(";
        osDefnName += pszGeomColForced;
        osDefnName += ")";
    }

    poFeatureDefn = new OGRPGTableFeatureDefn(this, osDefnName);
    SetDescription(poFeatureDefn->GetName());
    poFeatureDefn->Reference();

    // bSealFields = false because we do lazy resolution of fields
    poFeatureDefn->Seal(/* bSealFields = */ false);

    if (pszDescriptionIn != nullptr && !EQUAL(pszDescriptionIn, ""))
    {
        OGRLayer::SetMetadataItem("DESCRIPTION", pszDescriptionIn);
    }
}

//************************************************************************/
/*                          ~OGRPGTableLayer()                          */
/************************************************************************/

OGRPGTableLayer::~OGRPGTableLayer()

{
    if (bDeferredCreation)
        RunDeferredCreationIfNecessary();
    if (bCopyActive)
        EndCopy();
    UpdateSequenceIfNeeded();
    SerializeMetadata();

    CPLFree(pszSqlTableName);
    CPLFree(pszTableName);
    CPLFree(pszSqlGeomParentTableName);
    CPLFree(pszSchemaName);
    CPLFree(m_pszTableDescription);
    CPLFree(pszGeomColForced);
    CSLDestroy(papszOverrideColumnTypes);
}

/************************************************************************/
/*                              LoadMetadata()                          */
/************************************************************************/

void OGRPGTableLayer::LoadMetadata()
{
    if (m_bMetadataLoaded)
        return;
    m_bMetadataLoaded = true;

    if (!poDS->HasOgrSystemTablesMetadataTable())
        return;

    PGconn *hPGConn = poDS->GetPGConn();

    const std::string osSQL(
        CPLSPrintf("SELECT metadata FROM ogr_system_tables.metadata WHERE "
                   "schema_name = %s AND table_name = %s",
                   OGRPGEscapeString(hPGConn, pszSchemaName).c_str(),
                   OGRPGEscapeString(hPGConn, pszTableName).c_str()));
    auto poSqlLyr = poDS->ExecuteSQL(osSQL.c_str(), nullptr, nullptr);
    if (poSqlLyr)
    {
        auto poFeature =
            std::unique_ptr<OGRFeature>(poSqlLyr->GetNextFeature());
        if (poFeature)
        {
            if (poFeature->IsFieldSetAndNotNull(0))
            {
                const char *pszXML = poFeature->GetFieldAsString(0);
                if (pszXML)
                {
                    auto psRoot = CPLParseXMLString(pszXML);
                    if (psRoot)
                    {
                        oMDMD.XMLInit(psRoot, true);
                        CPLDestroyXMLNode(psRoot);
                    }
                }
            }
        }
        poDS->ReleaseResultSet(poSqlLyr);
    }
}

/************************************************************************/
/*                         SerializeMetadata()                          */
/************************************************************************/

void OGRPGTableLayer::SerializeMetadata()
{
    if (!m_bMetadataModified ||
        !CPLTestBool(CPLGetConfigOption("OGR_PG_ENABLE_METADATA", "YES")))
    {
        return;
    }

    PGconn *hPGConn = poDS->GetPGConn();
    CPLXMLNode *psMD = oMDMD.Serialize();

    if (psMD)
    {
        // Remove DESCRIPTION and OLMD_FID64 items from metadata

        CPLXMLNode *psPrev = nullptr;
        for (CPLXMLNode *psIter = psMD; psIter;)
        {
            CPLXMLNode *psNext = psIter->psNext;
            if (psIter->eType == CXT_Element &&
                strcmp(psIter->pszValue, "Metadata") == 0 &&
                CPLGetXMLNode(psIter, "domain") == nullptr)
            {
                bool bFoundInterestingItems = false;
                for (CPLXMLNode *psIter2 = psIter->psChild; psIter2;)
                {
                    CPLXMLNode *psNext2 = psIter2->psNext;
                    if (psIter2->eType == CXT_Element &&
                        strcmp(psIter2->pszValue, "MDI") == 0 &&
                        (EQUAL(CPLGetXMLValue(psIter2, "key", ""),
                               OLMD_FID64) ||
                         EQUAL(CPLGetXMLValue(psIter2, "key", ""),
                               "DESCRIPTION")))
                    {
                        CPLRemoveXMLChild(psIter, psIter2);
                    }
                    else
                    {
                        bFoundInterestingItems = true;
                    }
                    psIter2 = psNext2;
                }
                if (!bFoundInterestingItems)
                {
                    if (psPrev)
                        psPrev->psNext = psNext;
                    else
                        psMD = psNext;
                    psIter->psNext = nullptr;
                    CPLDestroyXMLNode(psIter);
                }
            }
            psIter = psNext;
            psPrev = psIter;
        }
    }

    const bool bIsUserTransactionActive = poDS->IsUserTransactionActive();
    {
        PGresult *hResult = OGRPG_PQexec(
            hPGConn, bIsUserTransactionActive
                         ? "SAVEPOINT ogr_system_tables_metadata_savepoint"
                         : "BEGIN");
        OGRPGClearResult(hResult);
    }

    if (psMD)
    {
        if (poDS->CreateMetadataTableIfNeeded() &&
            poDS->HasWritePermissionsOnMetadataTable())
        {
            CPLString osCommand;
            osCommand.Printf("DELETE FROM ogr_system_tables.metadata WHERE "
                             "schema_name = %s AND table_name = %s",
                             OGRPGEscapeString(hPGConn, pszSchemaName).c_str(),
                             OGRPGEscapeString(hPGConn, pszTableName).c_str());
            PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());
            OGRPGClearResult(hResult);

            CPLXMLNode *psRoot =
                CPLCreateXMLNode(nullptr, CXT_Element, "GDALMetadata");
            CPLAddXMLChild(psRoot, psMD);
            char *pszXML = CPLSerializeXMLTree(psRoot);
            // CPLDebug("PG", "Serializing %s", pszXML);

            osCommand.Printf(
                "INSERT INTO ogr_system_tables.metadata (schema_name, "
                "table_name, metadata) VALUES (%s, %s, %s)",
                OGRPGEscapeString(hPGConn, pszSchemaName).c_str(),
                OGRPGEscapeString(hPGConn, pszTableName).c_str(),
                OGRPGEscapeString(hPGConn, pszXML).c_str());
            hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());
            OGRPGClearResult(hResult);

            CPLDestroyXMLNode(psRoot);
            CPLFree(pszXML);
        }
    }
    else if (poDS->HasOgrSystemTablesMetadataTable() &&
             poDS->HasWritePermissionsOnMetadataTable())
    {
        CPLString osCommand;
        osCommand.Printf("DELETE FROM ogr_system_tables.metadata WHERE "
                         "schema_name = %s AND table_name = %s",
                         OGRPGEscapeString(hPGConn, pszSchemaName).c_str(),
                         OGRPGEscapeString(hPGConn, pszTableName).c_str());
        PGresult *hResult =
            OGRPG_PQexec(hPGConn, osCommand.c_str(), false, true);
        OGRPGClearResult(hResult);
    }

    {
        PGresult *hResult = OGRPG_PQexec(
            hPGConn,
            bIsUserTransactionActive
                ? "RELEASE SAVEPOINT ogr_system_tables_metadata_savepoint"
                : "COMMIT");
        OGRPGClearResult(hResult);
    }
}

/************************************************************************/
/*                          GetMetadataDomainList()                     */
/************************************************************************/

char **OGRPGTableLayer::GetMetadataDomainList()
{
    LoadMetadata();

    if (m_pszTableDescription == nullptr)
        GetMetadata();
    if (m_pszTableDescription != nullptr && m_pszTableDescription[0] != '\0')
        return CSLAddString(nullptr, "");
    return nullptr;
}

/************************************************************************/
/*                              GetMetadata()                           */
/************************************************************************/

char **OGRPGTableLayer::GetMetadata(const char *pszDomain)
{
    LoadMetadata();

    if ((pszDomain == nullptr || EQUAL(pszDomain, "")) &&
        m_pszTableDescription == nullptr)
    {
        PGconn *hPGConn = poDS->GetPGConn();
        CPLString osCommand;
        osCommand.Printf("SELECT d.description FROM pg_class c "
                         "JOIN pg_namespace n ON c.relnamespace=n.oid "
                         "JOIN pg_description d "
                         "ON d.objoid = c.oid AND d.classoid = "
                         "'pg_class'::regclass::oid AND d.objsubid = 0 "
                         "WHERE c.relname = %s AND n.nspname = %s AND "
                         "c.relkind in ('r', 'v') ",
                         OGRPGEscapeString(hPGConn, pszTableName).c_str(),
                         OGRPGEscapeString(hPGConn, pszSchemaName).c_str());
        PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());

        const char *pszDesc = nullptr;
        if (hResult && PGRES_TUPLES_OK == PQresultStatus(hResult) &&
            PQntuples(hResult) == 1)
        {
            pszDesc = PQgetvalue(hResult, 0, 0);
            if (pszDesc)
                OGRLayer::SetMetadataItem("DESCRIPTION", pszDesc);
        }
        m_pszTableDescription = CPLStrdup(pszDesc ? pszDesc : "");

        OGRPGClearResult(hResult);
    }

    return OGRLayer::GetMetadata(pszDomain);
}

/************************************************************************/
/*                            GetMetadataItem()                         */
/************************************************************************/

const char *OGRPGTableLayer::GetMetadataItem(const char *pszName,
                                             const char *pszDomain)
{
    LoadMetadata();

    GetMetadata(pszDomain);
    return OGRLayer::GetMetadataItem(pszName, pszDomain);
}

/************************************************************************/
/*                              SetMetadata()                           */
/************************************************************************/

CPLErr OGRPGTableLayer::SetMetadata(char **papszMD, const char *pszDomain)
{
    LoadMetadata();

    OGRLayer::SetMetadata(papszMD, pszDomain);
    m_bMetadataModified = true;

    if (!osForcedDescription.empty() &&
        (pszDomain == nullptr || EQUAL(pszDomain, "")))
    {
        OGRLayer::SetMetadataItem("DESCRIPTION", osForcedDescription);
    }

    if (!bDeferredCreation && (pszDomain == nullptr || EQUAL(pszDomain, "")))
    {
        const char *pszDescription = OGRLayer::GetMetadataItem("DESCRIPTION");
        if (pszDescription == nullptr)
            pszDescription = "";
        PGconn *hPGConn = poDS->GetPGConn();
        CPLString osCommand;

        osCommand.Printf(
            "COMMENT ON TABLE %s IS %s", pszSqlTableName,
            pszDescription[0] != '\0'
                ? OGRPGEscapeString(hPGConn, pszDescription).c_str()
                : "NULL");
        PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());
        OGRPGClearResult(hResult);

        CPLFree(m_pszTableDescription);
        m_pszTableDescription = CPLStrdup(pszDescription);
    }

    return CE_None;
}

/************************************************************************/
/*                            SetMetadataItem()                         */
/************************************************************************/

CPLErr OGRPGTableLayer::SetMetadataItem(const char *pszName,
                                        const char *pszValue,
                                        const char *pszDomain)
{
    LoadMetadata();

    if ((pszDomain == nullptr || EQUAL(pszDomain, "")) && pszName != nullptr &&
        EQUAL(pszName, "DESCRIPTION") && !osForcedDescription.empty())
    {
        pszValue = osForcedDescription;
    }

    OGRLayer::SetMetadataItem(pszName, pszValue, pszDomain);
    m_bMetadataModified = true;

    if (!bDeferredCreation && (pszDomain == nullptr || EQUAL(pszDomain, "")) &&
        pszName != nullptr && EQUAL(pszName, "DESCRIPTION"))
    {
        SetMetadata(GetMetadata());
    }

    return CE_None;
}

/************************************************************************/
/*                      SetForcedDescription()                          */
/************************************************************************/

void OGRPGTableLayer::SetForcedDescription(const char *pszDescriptionIn)
{
    osForcedDescription = pszDescriptionIn;
    CPLFree(m_pszTableDescription);
    m_pszTableDescription = CPLStrdup(pszDescriptionIn);
    SetMetadataItem("DESCRIPTION", osForcedDescription);
}

/************************************************************************/
/*                      SetGeometryInformation()                        */
/************************************************************************/

void OGRPGTableLayer::SetGeometryInformation(PGGeomColumnDesc *pasDesc,
                                             int nGeomFieldCount)
{
    // Flag must be set before instantiating geometry fields.
    bGeometryInformationSet = TRUE;
    auto oTemporaryUnsealer(poFeatureDefn->GetTemporaryUnsealer(false));

    for (int i = 0; i < nGeomFieldCount; i++)
    {
        auto poGeomFieldDefn =
            std::make_unique<OGRPGGeomFieldDefn>(this, pasDesc[i].pszName);
        poGeomFieldDefn->SetNullable(pasDesc[i].bNullable);
        poGeomFieldDefn->nSRSId = pasDesc[i].nSRID;
        poGeomFieldDefn->GeometryTypeFlags = pasDesc[i].GeometryTypeFlags;
        poGeomFieldDefn->ePostgisType = pasDesc[i].ePostgisType;
        if (pasDesc[i].pszGeomType != nullptr)
        {
            OGRwkbGeometryType eGeomType =
                OGRFromOGCGeomType(pasDesc[i].pszGeomType);
            if ((poGeomFieldDefn->GeometryTypeFlags & OGRGeometry::OGR_G_3D) &&
                (eGeomType != wkbUnknown))
                eGeomType = wkbSetZ(eGeomType);
            if ((poGeomFieldDefn->GeometryTypeFlags &
                 OGRGeometry::OGR_G_MEASURED) &&
                (eGeomType != wkbUnknown))
                eGeomType = wkbSetM(eGeomType);
            poGeomFieldDefn->SetType(eGeomType);
        }
        poFeatureDefn->AddGeomFieldDefn(std::move(poGeomFieldDefn));
    }
}

/************************************************************************/
/*                        ReadTableDefinition()                         */
/*                                                                      */
/*      Build a schema from the named table.  Done by querying the      */
/*      catalog.                                                        */
/************************************************************************/

int OGRPGTableLayer::ReadTableDefinition()

{
    PGconn *hPGConn = poDS->GetPGConn();

    if (bTableDefinitionValid >= 0)
        return bTableDefinitionValid;
    bTableDefinitionValid = FALSE;

    poDS->EndCopy();

    auto oTemporaryUnsealer(poFeatureDefn->GetTemporaryUnsealer());

    /* -------------------------------------------------------------------- */
    /*      Get the OID of the table.                                       */
    /* -------------------------------------------------------------------- */

    CPLString osCommand;
    osCommand.Printf("SELECT c.oid FROM pg_class c "
                     "JOIN pg_namespace n ON c.relnamespace=n.oid "
                     "WHERE c.relname = %s AND n.nspname = %s",
                     OGRPGEscapeString(hPGConn, pszTableName).c_str(),
                     OGRPGEscapeString(hPGConn, pszSchemaName).c_str());
    unsigned int nTableOID = 0;
    {
        PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());
        if (hResult && PGRES_TUPLES_OK == PQresultStatus(hResult))
        {
            if (PQntuples(hResult) == 1 && PQgetisnull(hResult, 0, 0) == false)
            {
                nTableOID = static_cast<unsigned>(
                    CPLAtoGIntBig(PQgetvalue(hResult, 0, 0)));
                OGRPGClearResult(hResult);
            }
            else
            {
                CPLDebug("PG", "Could not retrieve table oid for %s",
                         pszTableName);
                OGRPGClearResult(hResult);
                return FALSE;
            }
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s",
                     PQerrorMessage(hPGConn));
            return FALSE;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Identify the integer primary key.                               */
    /* -------------------------------------------------------------------- */

    osCommand.Printf(
        "SELECT a.attname, a.attnum, t.typname, "
        "t.typname = ANY(ARRAY['int2','int4','int8','serial','bigserial']) AS "
        "isfid "
        "FROM pg_attribute a "
        "JOIN pg_type t ON t.oid = a.atttypid "
        "JOIN pg_index i ON i.indrelid = a.attrelid "
        "WHERE a.attnum > 0 AND a.attrelid = %u "
        "AND i.indisprimary = 't' "
        "AND t.typname !~ '^geom' "
        "AND a.attnum = ANY(i.indkey) ORDER BY a.attnum",
        nTableOID);

    PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());

    if (hResult && PGRES_TUPLES_OK == PQresultStatus(hResult))
    {
        if (PQntuples(hResult) == 1 && PQgetisnull(hResult, 0, 0) == false)
        {
            /* Check if single-field PK can be represented as integer. */
            CPLString osValue(PQgetvalue(hResult, 0, 3));
            if (osValue == "t")
            {
                osPrimaryKey.Printf("%s", PQgetvalue(hResult, 0, 0));
                const char *pszFIDType = PQgetvalue(hResult, 0, 2);
                CPLDebug("PG", "Primary key name (FID): %s, type : %s",
                         osPrimaryKey.c_str(), pszFIDType);
                if (EQUAL(pszFIDType, "int8"))
                    OGRLayer::SetMetadataItem(OLMD_FID64, "YES");
            }
        }
        else if (PQntuples(hResult) > 1)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Multi-column primary key in \'%s\' detected but not "
                     "supported.",
                     pszTableName);
        }

        OGRPGClearResult(hResult);
        /* Zero tuples means no PK is defined, perfectly valid case. */
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s", PQerrorMessage(hPGConn));
    }

    /* -------------------------------------------------------------------- */
    /*      Fire off commands to get back the columns of the table.         */
    /* -------------------------------------------------------------------- */
    osCommand.Printf(
        "SELECT a.attname, t.typname, a.attlen,"
        "       format_type(a.atttypid,a.atttypmod), a.attnotnull, def.def, "
        "i.indisunique, descr.description%s "
        "FROM pg_attribute a "
        "JOIN pg_type t ON t.oid = a.atttypid "
        "LEFT JOIN "
        "(SELECT adrelid, adnum, pg_get_expr(adbin, adrelid) AS def FROM "
        "pg_attrdef) def "
        "ON def.adrelid = a.attrelid AND def.adnum = a.attnum "
        // Find unique constraints that are on a single column only
        "LEFT JOIN "
        "(SELECT DISTINCT indrelid, indkey, indisunique FROM pg_index WHERE "
        "indisunique) i "
        "ON i.indrelid = a.attrelid AND i.indkey[0] = a.attnum AND i.indkey[1] "
        "IS NULL "
        "LEFT JOIN pg_description descr "
        "ON descr.objoid = a.attrelid "
        "AND descr.classoid = 'pg_class'::regclass::oid "
        "AND descr.objsubid = a.attnum "
        "WHERE a.attnum > 0 AND a.attrelid = %u "
        "ORDER BY a.attnum",
        (poDS->sPostgreSQLVersion.nMajor >= 12 ? ", a.attgenerated" : ""),
        nTableOID);

    hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());

    if (!hResult || PQresultStatus(hResult) != PGRES_TUPLES_OK)
    {
        OGRPGClearResult(hResult);

        CPLError(CE_Failure, CPLE_AppDefined, "%s", PQerrorMessage(hPGConn));
        return bTableDefinitionValid;
    }

    if (PQntuples(hResult) == 0)
    {
        OGRPGClearResult(hResult);

        CPLDebug("PG", "No field definitions found for '%s', is it a table?",
                 pszTableName);
        return bTableDefinitionValid;
    }

    /* -------------------------------------------------------------------- */
    /*      Parse the returned table information.                           */
    /* -------------------------------------------------------------------- */
    for (int iRecord = 0; iRecord < PQntuples(hResult); iRecord++)
    {
        OGRFieldDefn oField(PQgetvalue(hResult, iRecord, 0), OFTString);

        const char *pszType = PQgetvalue(hResult, iRecord, 1);
        int nWidth = atoi(PQgetvalue(hResult, iRecord, 2));
        const char *pszFormatType = PQgetvalue(hResult, iRecord, 3);
        const char *pszNotNull = PQgetvalue(hResult, iRecord, 4);
        const char *pszDefault = PQgetisnull(hResult, iRecord, 5)
                                     ? nullptr
                                     : PQgetvalue(hResult, iRecord, 5);
        const char *pszIsUnique = PQgetvalue(hResult, iRecord, 6);
        const char *pszDescription = PQgetvalue(hResult, iRecord, 7);
        const char *pszGenerated = poDS->sPostgreSQLVersion.nMajor >= 12
                                       ? PQgetvalue(hResult, iRecord, 8)
                                       : "";

        if (pszNotNull && EQUAL(pszNotNull, "t"))
            oField.SetNullable(FALSE);
        if (pszIsUnique && EQUAL(pszIsUnique, "t"))
            oField.SetUnique(TRUE);

        if (EQUAL(oField.GetNameRef(), osPrimaryKey))
        {
            pszFIDColumn = CPLStrdup(oField.GetNameRef());
            CPLDebug("PG", "Using column '%s' as FID for table '%s'",
                     pszFIDColumn, pszTableName);
            continue;
        }
        else if (EQUAL(pszType, "geometry") || EQUAL(pszType, "geography") ||
                 EQUAL(oField.GetNameRef(), "WKB_GEOMETRY"))
        {
            const auto InitGeomField =
                [this, &pszType, &oField](OGRPGGeomFieldDefn *poGeomFieldDefn)
            {
                if (EQUAL(pszType, "geometry"))
                    poGeomFieldDefn->ePostgisType = GEOM_TYPE_GEOMETRY;
                else if (EQUAL(pszType, "geography"))
                {
                    poGeomFieldDefn->ePostgisType = GEOM_TYPE_GEOGRAPHY;
                    if (!(poDS->sPostGISVersion.nMajor >= 3 ||
                          (poDS->sPostGISVersion.nMajor == 2 &&
                           poDS->sPostGISVersion.nMinor >= 2)))
                    {
                        // EPSG:4326 was a requirement for geography before
                        // PostGIS 2.2
                        poGeomFieldDefn->nSRSId = 4326;
                    }
                }
                else
                {
                    poGeomFieldDefn->ePostgisType = GEOM_TYPE_WKB;
                    if (EQUAL(pszType, "OID"))
                        bWkbAsOid = TRUE;
                }
                poGeomFieldDefn->SetNullable(oField.IsNullable());
            };

            if (!bGeometryInformationSet)
            {
                if (pszGeomColForced == nullptr ||
                    EQUAL(pszGeomColForced, oField.GetNameRef()))
                {
                    auto poGeomFieldDefn = std::make_unique<OGRPGGeomFieldDefn>(
                        this, oField.GetNameRef());
                    InitGeomField(poGeomFieldDefn.get());
                    poFeatureDefn->AddGeomFieldDefn(std::move(poGeomFieldDefn));
                }
            }
            else
            {
                int idx = poFeatureDefn->GetGeomFieldIndex(oField.GetNameRef());
                if (idx >= 0)
                {
                    auto poGeomFieldDefn = poFeatureDefn->GetGeomFieldDefn(idx);
                    InitGeomField(poGeomFieldDefn);
                }
            }

            continue;
        }

        OGRPGCommonLayerSetType(oField, pszType, pszFormatType, nWidth);

        if (pszDefault)
        {
            OGRPGCommonLayerNormalizeDefault(&oField, pszDefault);
        }
        if (pszDescription)
            oField.SetComment(pszDescription);

        oField.SetGenerated(pszGenerated != nullptr && pszGenerated[0] != '\0');

        // CPLDebug("PG", "name=%s, type=%s", oField.GetNameRef(), pszType);
        poFeatureDefn->AddFieldDefn(&oField);
    }

    OGRPGClearResult(hResult);

    bTableDefinitionValid = TRUE;

    ResetReading();

    /* If geometry type, SRID, etc... have always been set by
     * SetGeometryInformation() */
    /* no need to issue a new SQL query. Just record the geom type in the layer
     * definition */
    if (bGeometryInformationSet)
    {
        return TRUE;
    }
    bGeometryInformationSet = TRUE;

    // get layer geometry type (for PostGIS dataset)
    for (int iField = 0; iField < poFeatureDefn->GetGeomFieldCount(); iField++)
    {
        OGRPGGeomFieldDefn *poGeomFieldDefn =
            poFeatureDefn->GetGeomFieldDefn(iField);

        /* Get the geometry type and dimensions from the table, or */
        /* from its parents if it is a derived table, or from the parent of the
         * parent, etc.. */
        int bGoOn = poDS->m_bHasGeometryColumns;
        const bool bHasPostGISGeometry =
            (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOMETRY);

        while (bGoOn)
        {
            const CPLString osEscapedThisOrParentTableName(OGRPGEscapeString(
                hPGConn, (pszSqlGeomParentTableName) ? pszSqlGeomParentTableName
                                                     : pszTableName));
            osCommand.Printf("SELECT type, coord_dimension, srid FROM %s WHERE "
                             "f_table_name = %s",
                             (bHasPostGISGeometry) ? "geometry_columns"
                                                   : "geography_columns",
                             osEscapedThisOrParentTableName.c_str());

            osCommand += CPLString().Printf(
                " AND %s=%s",
                (bHasPostGISGeometry) ? "f_geometry_column"
                                      : "f_geography_column",
                OGRPGEscapeString(hPGConn, poGeomFieldDefn->GetNameRef())
                    .c_str());

            osCommand += CPLString().Printf(
                " AND f_table_schema = %s",
                OGRPGEscapeString(hPGConn, pszSchemaName).c_str());

            hResult = OGRPG_PQexec(hPGConn, osCommand);

            if (hResult && PQntuples(hResult) == 1 &&
                !PQgetisnull(hResult, 0, 0))
            {
                const char *pszType = PQgetvalue(hResult, 0, 0);

                int dim = atoi(PQgetvalue(hResult, 0, 1));
                bool bHasM = pszType[strlen(pszType) - 1] == 'M';
                int GeometryTypeFlags = 0;
                if (dim == 3)
                {
                    if (bHasM)
                        GeometryTypeFlags |= OGRGeometry::OGR_G_MEASURED;
                    else
                        GeometryTypeFlags |= OGRGeometry::OGR_G_3D;
                }
                else if (dim == 4)
                    GeometryTypeFlags |=
                        OGRGeometry::OGR_G_3D | OGRGeometry::OGR_G_MEASURED;

                int nSRSId = atoi(PQgetvalue(hResult, 0, 2));

                poGeomFieldDefn->GeometryTypeFlags = GeometryTypeFlags;
                if (nSRSId > 0)
                    poGeomFieldDefn->nSRSId = nSRSId;
                OGRwkbGeometryType eGeomType = OGRFromOGCGeomType(pszType);
                if (poGeomFieldDefn->GeometryTypeFlags &
                        OGRGeometry::OGR_G_3D &&
                    eGeomType != wkbUnknown)
                    eGeomType = wkbSetZ(eGeomType);
                if (poGeomFieldDefn->GeometryTypeFlags &
                        OGRGeometry::OGR_G_MEASURED &&
                    eGeomType != wkbUnknown)
                    eGeomType = wkbSetM(eGeomType);
                poGeomFieldDefn->SetType(eGeomType);

                bGoOn = FALSE;
            }
            else
            {
                /* Fetch the name of the parent table */
                osCommand.Printf(
                    "SELECT pg_class.relname FROM pg_class WHERE oid = "
                    "(SELECT pg_inherits.inhparent FROM pg_inherits WHERE "
                    "inhrelid = "
                    "(SELECT c.oid FROM pg_class c, pg_namespace n "
                    "WHERE c.relname = %s AND c.relnamespace=n.oid AND "
                    "n.nspname = %s))",
                    osEscapedThisOrParentTableName.c_str(),
                    OGRPGEscapeString(hPGConn, pszSchemaName).c_str());

                OGRPGClearResult(hResult);
                hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());

                if (hResult && PQntuples(hResult) == 1 &&
                    !PQgetisnull(hResult, 0, 0))
                {
                    CPLFree(pszSqlGeomParentTableName);
                    pszSqlGeomParentTableName =
                        CPLStrdup(PQgetvalue(hResult, 0, 0));
                }
                else
                {
                    /* No more parent : stop recursion */
                    bGoOn = FALSE;
                }
            }

            OGRPGClearResult(hResult);
        }
    }

    return bTableDefinitionValid;
}

/************************************************************************/
/*                         SetTableDefinition()                         */
/************************************************************************/

void OGRPGTableLayer::SetTableDefinition(const char *pszFIDColumnName,
                                         const char *pszGFldName,
                                         OGRwkbGeometryType eType,
                                         const char *pszGeomType, int nSRSId,
                                         int GeometryTypeFlags)
{
    bTableDefinitionValid = TRUE;
    bGeometryInformationSet = TRUE;
    pszFIDColumn = CPLStrdup(pszFIDColumnName);
    auto oTemporaryUnsealer(poFeatureDefn->GetTemporaryUnsealer());
    poFeatureDefn->SetGeomType(wkbNone);
    if (eType != wkbNone)
    {
        auto poGeomFieldDefn =
            std::make_unique<OGRPGGeomFieldDefn>(this, pszGFldName);
        poGeomFieldDefn->SetType(eType);
        poGeomFieldDefn->GeometryTypeFlags = GeometryTypeFlags;

        if (EQUAL(pszGeomType, "geometry"))
        {
            poGeomFieldDefn->ePostgisType = GEOM_TYPE_GEOMETRY;
            poGeomFieldDefn->nSRSId = nSRSId;
        }
        else if (EQUAL(pszGeomType, "geography"))
        {
            poGeomFieldDefn->ePostgisType = GEOM_TYPE_GEOGRAPHY;
            poGeomFieldDefn->nSRSId = nSRSId;
        }
        else
        {
            poGeomFieldDefn->ePostgisType = GEOM_TYPE_WKB;
            if (EQUAL(pszGeomType, "OID"))
                bWkbAsOid = TRUE;
        }
        poFeatureDefn->AddGeomFieldDefn(std::move(poGeomFieldDefn));
    }
    else if (pszGFldName != nullptr)
    {
        m_osFirstGeometryFieldName = pszGFldName;
    }
    m_osLCOGeomType = pszGeomType;
}

/************************************************************************/
/*                         ISetSpatialFilter()                          */
/************************************************************************/

OGRErr OGRPGTableLayer::ISetSpatialFilter(int iGeomField,
                                          const OGRGeometry *poGeomIn)

{
    m_iGeomFieldFilter = iGeomField;

    if (InstallFilter(poGeomIn))
    {
        BuildWhere();

        ResetReading();
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                             BuildWhere()                             */
/*                                                                      */
/*      Build the WHERE statement appropriate to the current set of     */
/*      criteria (spatial and attribute queries).                       */
/************************************************************************/

void OGRPGTableLayer::BuildWhere()

{
    osWHERE = "";
    OGRPGGeomFieldDefn *poGeomFieldDefn = nullptr;
    if (poFeatureDefn->GetGeomFieldCount() != 0)
        poGeomFieldDefn = poFeatureDefn->GetGeomFieldDefn(m_iGeomFieldFilter);

    if (m_poFilterGeom != nullptr && poGeomFieldDefn != nullptr &&
        poDS->sPostGISVersion.nMajor >= 0 &&
        (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOMETRY ||
         poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOGRAPHY))
    {
        char szBox3D_1[128];
        char szBox3D_2[128];
        OGREnvelope sEnvelope;

        m_poFilterGeom->getEnvelope(&sEnvelope);
        if (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOGRAPHY)
        {
            if (sEnvelope.MinX < -180.0)
                sEnvelope.MinX = -180.0;
            if (sEnvelope.MinY < -90.0)
                sEnvelope.MinY = -90.0;
            if (sEnvelope.MaxX > 180.0)
                sEnvelope.MaxX = 180.0;
            if (sEnvelope.MaxY > 90.0)
                sEnvelope.MaxY = 90.0;
        }
        CPLsnprintf(szBox3D_1, sizeof(szBox3D_1), "%.17g %.17g", sEnvelope.MinX,
                    sEnvelope.MinY);
        CPLsnprintf(szBox3D_2, sizeof(szBox3D_2), "%.17g %.17g", sEnvelope.MaxX,
                    sEnvelope.MaxY);
        osWHERE.Printf(
            "WHERE %s && ST_SetSRID('BOX3D(%s, %s)'::box3d,%d) ",
            OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef()).c_str(),
            szBox3D_1, szBox3D_2, poGeomFieldDefn->nSRSId);
    }

    if (!osQuery.empty())
    {
        if (osWHERE.empty())
        {
            osWHERE.Printf("WHERE %s ", osQuery.c_str());
        }
        else
        {
            osWHERE += "AND (";
            osWHERE += osQuery;
            osWHERE += ")";
        }
    }
}

/************************************************************************/
/*                      BuildFullQueryStatement()                       */
/************************************************************************/

void OGRPGTableLayer::BuildFullQueryStatement()

{
    CPLString osFields = BuildFields();
    if (pszQueryStatement != nullptr)
    {
        CPLFree(pszQueryStatement);
        pszQueryStatement = nullptr;
    }
    pszQueryStatement = static_cast<char *>(CPLMalloc(
        osFields.size() + osWHERE.size() + strlen(pszSqlTableName) + 40));
    snprintf(pszQueryStatement,
             osFields.size() + osWHERE.size() + strlen(pszSqlTableName) + 40,
             "SELECT %s FROM %s %s", osFields.c_str(), pszSqlTableName,
             osWHERE.c_str());
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void OGRPGTableLayer::ResetReading()

{
    if (bInResetReading)
        return;
    bInResetReading = TRUE;

    if (bDeferredCreation)
        RunDeferredCreationIfNecessary();
    poDS->EndCopy();
    bUseCopyByDefault = FALSE;

    BuildFullQueryStatement();

    OGRPGLayer::ResetReading();

    bInResetReading = FALSE;
}

/************************************************************************/
/*                           GetNextFeature()                           */
/************************************************************************/

OGRFeature *OGRPGTableLayer::GetNextFeature()

{
    if (bDeferredCreation && RunDeferredCreationIfNecessary() != OGRERR_NONE)
        return nullptr;
    poDS->EndCopy();

    if (pszQueryStatement == nullptr)
        ResetReading();

    OGRPGGeomFieldDefn *poGeomFieldDefn = nullptr;
    if (poFeatureDefn->GetGeomFieldCount() != 0)
        poGeomFieldDefn = poFeatureDefn->GetGeomFieldDefn(m_iGeomFieldFilter);
    poFeatureDefn->GetFieldCount();

    while (true)
    {
        OGRFeature *poFeature = GetNextRawFeature();
        if (poFeature == nullptr)
            return nullptr;

        /* We just have to look if there is a geometry filter */
        /* If there's a PostGIS geometry column, the spatial filter */
        /* is already taken into account in the select request */
        /* The attribute filter is always taken into account by the select
         * request */
        if (m_poFilterGeom == nullptr || poGeomFieldDefn == nullptr ||
            poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOMETRY ||
            poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOGRAPHY ||
            FilterGeometry(poFeature->GetGeomFieldRef(m_iGeomFieldFilter)))
        {
            if (iFIDAsRegularColumnIndex >= 0)
            {
                poFeature->SetField(iFIDAsRegularColumnIndex,
                                    poFeature->GetFID());
            }
            return poFeature;
        }

        delete poFeature;
    }
}

/************************************************************************/
/*                            BuildFields()                             */
/*                                                                      */
/*      Build list of fields to fetch, performing any required          */
/*      transformations (such as on geometry).                          */
/************************************************************************/

CPLString OGRPGTableLayer::BuildFields()

{
    int i = 0;
    CPLString osFieldList;

    poFeatureDefn->GetFieldCount();

    if (pszFIDColumn != nullptr &&
        poFeatureDefn->GetFieldIndex(pszFIDColumn) == -1)
    {
        osFieldList += OGRPGEscapeColumnName(pszFIDColumn);
    }

    for (i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
    {
        OGRPGGeomFieldDefn *poGeomFieldDefn =
            poFeatureDefn->GetGeomFieldDefn(i);
        CPLString osEscapedGeom =
            OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef());

        if (!osFieldList.empty())
            osFieldList += ", ";

        if (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOMETRY)
        {
            if (!poDS->HavePostGIS() || poDS->bUseBinaryCursor)
            {
                osFieldList += osEscapedGeom;
            }
            else if (CPLTestBool(CPLGetConfigOption("PG_USE_BASE64", "NO")))
            {
                osFieldList += "encode(ST_AsEWKB(";
                osFieldList += osEscapedGeom;
                osFieldList += "), 'base64') AS ";
                osFieldList += OGRPGEscapeColumnName(
                    CPLSPrintf("EWKBBase64_%s", poGeomFieldDefn->GetNameRef()));
            }
            else
            {
                /* This will return EWKB in an hex encoded form */
                osFieldList += osEscapedGeom;
            }
        }
        else if (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOGRAPHY)
        {
#if defined(BINARY_CURSOR_ENABLED)
            if (poDS->bUseBinaryCursor)
            {
                osFieldList += "ST_AsBinary(";
                osFieldList += osEscapedGeom;
                osFieldList += ") AS";
                osFieldList += OGRPGEscapeColumnName(
                    CPLSPrintf("AsBinary_%s", poGeomFieldDefn->GetNameRef()));
            }
            else
#endif
                if (CPLTestBool(CPLGetConfigOption("PG_USE_BASE64", "NO")))
            {
                osFieldList += "encode(ST_AsEWKB(";
                osFieldList += osEscapedGeom;
                osFieldList += "::geometry), 'base64') AS ";
                osFieldList += OGRPGEscapeColumnName(
                    CPLSPrintf("EWKBBase64_%s", poGeomFieldDefn->GetNameRef()));
            }
            else
            {
                osFieldList += osEscapedGeom;
            }
        }
        else
        {
            osFieldList += osEscapedGeom;
        }
    }

    for (i = 0; i < poFeatureDefn->GetFieldCount(); i++)
    {
        const char *pszName = poFeatureDefn->GetFieldDefn(i)->GetNameRef();

        if (!osFieldList.empty())
            osFieldList += ", ";

#if defined(BINARY_CURSOR_ENABLED)
        /* With a binary cursor, it is not possible to get the time zone */
        /* of a timestamptz column. So we fallback to asking it in text mode */
        if (poDS->bUseBinaryCursor &&
            poFeatureDefn->GetFieldDefn(i)->GetType() == OFTDateTime)
        {
            osFieldList += "CAST (";
            osFieldList += OGRPGEscapeColumnName(pszName);
            osFieldList += " AS text)";
        }
        else
#endif
        {
            osFieldList += OGRPGEscapeColumnName(pszName);
        }
    }

    return osFieldList;
}

/************************************************************************/
/*                         SetAttributeFilter()                         */
/************************************************************************/

OGRErr OGRPGTableLayer::SetAttributeFilter(const char *pszQuery)

{
    CPLFree(m_pszAttrQueryString);
    m_pszAttrQueryString = (pszQuery) ? CPLStrdup(pszQuery) : nullptr;

    if (pszQuery == nullptr)
        osQuery = "";
    else
        osQuery = pszQuery;

    BuildWhere();

    ResetReading();

    return OGRERR_NONE;
}

/************************************************************************/
/*                           DeleteFeature()                            */
/************************************************************************/

OGRErr OGRPGTableLayer::DeleteFeature(GIntBig nFID)

{
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;

    GetLayerDefn()->GetFieldCount();

    if (!bUpdateAccess)
    {
        CPLError(CE_Failure, CPLE_NotSupported, UNSUPPORTED_OP_READ_ONLY,
                 "DeleteFeature");
        return OGRERR_FAILURE;
    }

    if (bDeferredCreation && RunDeferredCreationIfNecessary() != OGRERR_NONE)
        return OGRERR_FAILURE;
    poDS->EndCopy();
    bAutoFIDOnCreateViaCopy = FALSE;

    /* -------------------------------------------------------------------- */
    /*      We can only delete features if we have a well defined FID       */
    /*      column to target.                                               */
    /* -------------------------------------------------------------------- */
    if (pszFIDColumn == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "DeleteFeature(" CPL_FRMT_GIB
                 ") failed.  Unable to delete features in tables without\n"
                 "a recognised FID column.",
                 nFID);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Form the statement to drop the record.                          */
    /* -------------------------------------------------------------------- */
    osCommand.Printf("DELETE FROM %s WHERE %s = " CPL_FRMT_GIB, pszSqlTableName,
                     OGRPGEscapeColumnName(pszFIDColumn).c_str(), nFID);

    /* -------------------------------------------------------------------- */
    /*      Execute the delete.                                             */
    /* -------------------------------------------------------------------- */
    OGRErr eErr;

    PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);

    if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "DeleteFeature() DELETE statement failed.\n%s",
                 PQerrorMessage(hPGConn));

        eErr = OGRERR_FAILURE;
    }
    else
    {
        if (EQUAL(PQcmdStatus(hResult), "DELETE 0"))
            eErr = OGRERR_NON_EXISTING_FEATURE;
        else
            eErr = OGRERR_NONE;
    }

    OGRPGClearResult(hResult);

    return eErr;
}

/************************************************************************/
/*                             ISetFeature()                             */
/*                                                                      */
/*      SetFeature() is implemented by an UPDATE SQL command            */
/************************************************************************/

OGRErr OGRPGTableLayer::ISetFeature(OGRFeature *poFeature)

{
    return IUpdateFeature(poFeature, -1, nullptr, -1, nullptr, false);
}

/************************************************************************/
/*                           UpdateFeature()                            */
/************************************************************************/

OGRErr OGRPGTableLayer::IUpdateFeature(OGRFeature *poFeature,
                                       int nUpdatedFieldsCount,
                                       const int *panUpdatedFieldsIdx,
                                       int nUpdatedGeomFieldsCount,
                                       const int *panUpdatedGeomFieldsIdx,
                                       bool /* bUpdateStyleString*/)

{
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;
    bool bNeedComma = false;
    OGRErr eErr = OGRERR_FAILURE;

    GetLayerDefn()->GetFieldCount();

    const char *pszMethodName =
        nUpdatedFieldsCount >= 0 ? "UpdateFeature" : "SetFeature";
    if (!bUpdateAccess)
    {
        CPLError(CE_Failure, CPLE_NotSupported, UNSUPPORTED_OP_READ_ONLY,
                 pszMethodName);
        return OGRERR_FAILURE;
    }

    if (bDeferredCreation && RunDeferredCreationIfNecessary() != OGRERR_NONE)
        return OGRERR_FAILURE;
    poDS->EndCopy();

    if (nullptr == poFeature)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "NULL pointer to OGRFeature passed to %s().", pszMethodName);
        return eErr;
    }

    if (poFeature->GetFID() == OGRNullFID)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "FID required on features given to %s().", pszMethodName);
        return eErr;
    }

    if (pszFIDColumn == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unable to update features in tables without\n"
                 "a recognised FID column.");
        return eErr;
    }

    /* In case the FID column has also been created as a regular field */
    if (iFIDAsRegularColumnIndex >= 0)
    {
        if (!poFeature->IsFieldSetAndNotNull(iFIDAsRegularColumnIndex) ||
            poFeature->GetFieldAsInteger64(iFIDAsRegularColumnIndex) !=
                poFeature->GetFID())
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Inconsistent values of FID and field of same name");
            return OGRERR_FAILURE;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Form the UPDATE command.                                        */
    /* -------------------------------------------------------------------- */
    osCommand.Printf("UPDATE %s SET ", pszSqlTableName);

    /* Set the geometry */
    const int nGeomStop = nUpdatedGeomFieldsCount >= 0
                              ? nUpdatedGeomFieldsCount
                              : poFeatureDefn->GetGeomFieldCount();
    for (int i = 0; i < nGeomStop; i++)
    {
        const int iField =
            nUpdatedGeomFieldsCount >= 0 ? panUpdatedGeomFieldsIdx[i] : i;
        OGRPGGeomFieldDefn *poGeomFieldDefn =
            poFeatureDefn->GetGeomFieldDefn(iField);
        OGRGeometry *poGeom = poFeature->GetGeomFieldRef(iField);
        if (poGeomFieldDefn->ePostgisType == GEOM_TYPE_WKB)
        {
            if (bNeedComma)
                osCommand += ", ";
            else
                bNeedComma = true;

            osCommand += OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef());
            osCommand += " = ";
            if (poGeom != nullptr)
            {
                if (!bWkbAsOid)
                {
                    char *pszBytea =
                        GeometryToBYTEA(poGeom, poDS->sPostGISVersion.nMajor,
                                        poDS->sPostGISVersion.nMinor);

                    if (pszBytea != nullptr)
                    {
                        osCommand += "E'";
                        osCommand += pszBytea;
                        osCommand += '\'';
                        CPLFree(pszBytea);
                    }
                    else
                        osCommand += "NULL";
                }
                else
                {
                    Oid oid = GeometryToOID(poGeom);

                    if (oid != 0)
                    {
                        osCommand += CPLString().Printf("'%d' ", oid);
                    }
                    else
                        osCommand += "NULL";
                }
            }
            else
                osCommand += "NULL";
        }
        else if (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOGRAPHY ||
                 poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOMETRY)
        {
            if (bNeedComma)
                osCommand += ", ";
            else
                bNeedComma = true;

            osCommand += OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef());
            osCommand += " = ";
            if (poGeom != nullptr)
            {
                poGeom->closeRings();
                poGeom->set3D(poGeomFieldDefn->GeometryTypeFlags &
                              OGRGeometry::OGR_G_3D);
                poGeom->setMeasured(poGeomFieldDefn->GeometryTypeFlags &
                                    OGRGeometry::OGR_G_MEASURED);
            }

            if (poGeom != nullptr)
            {
                char *pszHexEWKB = OGRGeometryToHexEWKB(
                    poGeom, poGeomFieldDefn->nSRSId,
                    poDS->sPostGISVersion.nMajor, poDS->sPostGISVersion.nMinor);
                if (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOGRAPHY)
                    osCommand +=
                        CPLString().Printf("'%s'::GEOGRAPHY", pszHexEWKB);
                else
                    osCommand +=
                        CPLString().Printf("'%s'::GEOMETRY", pszHexEWKB);
                CPLFree(pszHexEWKB);
            }
            else
                osCommand += "NULL";
        }
    }

    const int nStop = nUpdatedFieldsCount >= 0 ? nUpdatedFieldsCount
                                               : poFeatureDefn->GetFieldCount();
    for (int i = 0; i < nStop; i++)
    {
        const int iField =
            nUpdatedFieldsCount >= 0 ? panUpdatedFieldsIdx[i] : i;
        if (iFIDAsRegularColumnIndex == iField)
            continue;
        if (!poFeature->IsFieldSet(iField))
            continue;
        if (poFeature->GetDefnRef()->GetFieldDefn(iField)->IsGenerated())
            continue;

        if (bNeedComma)
            osCommand += ", ";
        else
            bNeedComma = true;

        osCommand += OGRPGEscapeColumnName(
            poFeatureDefn->GetFieldDefn(iField)->GetNameRef());
        osCommand += " = ";

        if (poFeature->IsFieldNull(iField))
        {
            osCommand += "NULL";
        }
        else
        {
            OGRPGCommonAppendFieldValue(osCommand, poFeature, iField,
                                        OGRPGEscapeString, hPGConn);
        }
    }
    if (!bNeedComma)  // nothing to do
        return OGRERR_NONE;

    /* Add the WHERE clause */
    osCommand += " WHERE ";
    osCommand += OGRPGEscapeColumnName(pszFIDColumn);
    osCommand += +" = ";
    osCommand += CPLString().Printf(CPL_FRMT_GIB, poFeature->GetFID());

    /* -------------------------------------------------------------------- */
    /*      Execute the update.                                             */
    /* -------------------------------------------------------------------- */
    PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
    if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "UPDATE command for feature " CPL_FRMT_GIB
                 " failed.\n%s\nCommand: %s",
                 poFeature->GetFID(), PQerrorMessage(hPGConn),
                 osCommand.c_str());

        OGRPGClearResult(hResult);

        return OGRERR_FAILURE;
    }

    if (EQUAL(PQcmdStatus(hResult), "UPDATE 0"))
        eErr = OGRERR_NON_EXISTING_FEATURE;
    else
        eErr = OGRERR_NONE;

    OGRPGClearResult(hResult);

    return eErr;
}

/************************************************************************/
/*                           ICreateFeature()                            */
/************************************************************************/

OGRErr OGRPGTableLayer::ICreateFeature(OGRFeature *poFeature)
{
    GetLayerDefn()->GetFieldCount();

    if (!bUpdateAccess)
    {
        CPLError(CE_Failure, CPLE_NotSupported, UNSUPPORTED_OP_READ_ONLY,
                 "CreateFeature");
        return OGRERR_FAILURE;
    }

    if (nullptr == poFeature)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "NULL pointer to OGRFeature passed to CreateFeature().");
        return OGRERR_FAILURE;
    }

    if (bDeferredCreation && RunDeferredCreationIfNecessary() != OGRERR_NONE)
        return OGRERR_FAILURE;

    /* In case the FID column has also been created as a regular field */
    GIntBig nFID = poFeature->GetFID();
    if (iFIDAsRegularColumnIndex >= 0)
    {
        if (nFID == OGRNullFID)
        {
            if (poFeature->IsFieldSetAndNotNull(iFIDAsRegularColumnIndex))
            {
                poFeature->SetFID(
                    poFeature->GetFieldAsInteger64(iFIDAsRegularColumnIndex));
            }
        }
        else
        {
            if (!poFeature->IsFieldSetAndNotNull(iFIDAsRegularColumnIndex) ||
                poFeature->GetFieldAsInteger64(iFIDAsRegularColumnIndex) !=
                    nFID)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Inconsistent values of FID and field of same name");
                return OGRERR_FAILURE;
            }
        }
    }

    /* Auto-promote FID column to 64bit if necessary */
    if (pszFIDColumn != nullptr && !CPL_INT64_FITS_ON_INT32(nFID) &&
        OGRLayer::GetMetadataItem(OLMD_FID64) == nullptr)
    {
        poDS->EndCopy();

        CPLString osCommand;
        osCommand.Printf("ALTER TABLE %s ALTER COLUMN %s TYPE INT8",
                         pszSqlTableName,
                         OGRPGEscapeColumnName(pszFIDColumn).c_str());
        PGconn *hPGConn = poDS->GetPGConn();
        PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
        if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s\n%s", osCommand.c_str(),
                     PQerrorMessage(hPGConn));

            OGRPGClearResult(hResult);

            return OGRERR_FAILURE;
        }
        OGRPGClearResult(hResult);

        OGRLayer::SetMetadataItem(OLMD_FID64, "YES");
    }

    if (bFirstInsertion)
    {
        bFirstInsertion = FALSE;
        if (CPLTestBool(CPLGetConfigOption("OGR_TRUNCATE", "NO")))
        {
            PGconn *hPGConn = poDS->GetPGConn();
            CPLString osCommand;

            osCommand.Printf("TRUNCATE TABLE %s", pszSqlTableName);
            PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());
            OGRPGClearResult(hResult);
        }
    }

    // We avoid testing the config option too often.
    if (bUseCopy == USE_COPY_UNSET)
        bUseCopy = CPLTestBool(CPLGetConfigOption("PG_USE_COPY", "NO"));

    OGRErr eErr;
    if (!bUseCopy)
    {
        eErr = CreateFeatureViaInsert(poFeature);
    }
    else
    {
        /* If there's a unset field with a default value, then we must use */
        /* a specific INSERT statement to avoid unset fields to be bound to NULL
         */
        bool bHasDefaultValue = false;
        const int nFieldCount = poFeatureDefn->GetFieldCount();
        for (int iField = 0; iField < nFieldCount; iField++)
        {
            if (!poFeature->IsFieldSet(iField) &&
                poFeature->GetFieldDefnRef(iField)->GetDefault() != nullptr)
            {
                bHasDefaultValue = true;
                break;
            }
        }
        if (bHasDefaultValue)
        {
            eErr = CreateFeatureViaInsert(poFeature);
        }
        else
        {
            bool bFIDSet =
                (pszFIDColumn != nullptr && poFeature->GetFID() != OGRNullFID);
            if (bCopyActive && bFIDSet != bFIDColumnInCopyFields)
            {
                eErr = CreateFeatureViaInsert(poFeature);
            }
            else if (!bCopyActive && poFeatureDefn->GetFieldCount() == 0 &&
                     poFeatureDefn->GetGeomFieldCount() == 0 && !bFIDSet)
            {
                eErr = CreateFeatureViaInsert(poFeature);
            }
            else
            {
                if (!bCopyActive)
                {
                    /* This is a heuristics. If the first feature to be copied
                     * has a */
                    /* FID set (and that a FID column has been identified), then
                     * we will */
                    /* try to copy FID values from features. Otherwise, we will
                     * not */
                    /* do and assume that the FID column is an autoincremented
                     * column. */
                    bFIDColumnInCopyFields = bFIDSet;
                    bNeedToUpdateSequence = bFIDSet;
                }

                eErr = CreateFeatureViaCopy(poFeature);
                if (bFIDSet)
                    bAutoFIDOnCreateViaCopy = FALSE;
                if (eErr == OGRERR_NONE && bAutoFIDOnCreateViaCopy)
                {
                    poFeature->SetFID(++iNextShapeId);
                }
            }
        }
    }

    if (eErr == OGRERR_NONE && iFIDAsRegularColumnIndex >= 0)
    {
        poFeature->SetField(iFIDAsRegularColumnIndex, poFeature->GetFID());
    }

    return eErr;
}

/************************************************************************/
/*                       OGRPGEscapeColumnName( )                       */
/************************************************************************/

CPLString OGRPGEscapeColumnName(const char *pszColumnName)
{
    CPLString osStr = "\"";

    char ch = '\0';
    for (int i = 0; (ch = pszColumnName[i]) != '\0'; i++)
    {
        if (ch == '"')
            osStr.append(1, ch);
        osStr.append(1, ch);
    }

    osStr += "\"";

    return osStr;
}

/************************************************************************/
/*                         OGRPGEscapeString( )                         */
/************************************************************************/

CPLString OGRPGEscapeString(void *hPGConnIn, const char *pszStrValue,
                            int nMaxLength, const char *pszTableName,
                            const char *pszFieldName)
{
    PGconn *hPGConn = reinterpret_cast<PGconn *>(hPGConnIn);

    size_t nSrcLen = strlen(pszStrValue);
    const size_t nSrcLenUTF = CPLStrlenUTF8Ex(pszStrValue);

    if (nMaxLength > 0 && nSrcLenUTF > static_cast<size_t>(nMaxLength))
    {
        CPLDebug("PG", "Truncated %s.%s field value '%s' to %d characters.",
                 pszTableName, pszFieldName, pszStrValue, nMaxLength);

        size_t iUTF8Char = 0;
        for (size_t iChar = 0; iChar < nSrcLen; iChar++)
        {
            if ((static_cast<unsigned char>(pszStrValue[iChar]) & 0xc0) != 0x80)
            {
                if (iUTF8Char == static_cast<size_t>(nMaxLength))
                {
                    nSrcLen = iChar;
                    break;
                }
                iUTF8Char++;
            }
        }
    }

    char *pszDestStr = static_cast<char *>(CPLMalloc(2 * nSrcLen + 1));

    CPLString osCommand;

    /* We need to quote and escape string fields. */
    osCommand += "'";

    int nError = 0;
    PQescapeStringConn(hPGConn, pszDestStr, pszStrValue, nSrcLen, &nError);
    if (nError == 0)
        osCommand += pszDestStr;
    else
        CPLError(CE_Warning, CPLE_AppDefined,
                 "PQescapeString(): %s\n"
                 "  input: '%s'\n"
                 "    got: '%s'\n",
                 PQerrorMessage(hPGConn), pszStrValue, pszDestStr);

    CPLFree(pszDestStr);

    osCommand += "'";

    return osCommand;
}

/************************************************************************/
/*                       CreateFeatureViaInsert()                       */
/************************************************************************/

OGRErr OGRPGTableLayer::CreateFeatureViaInsert(OGRFeature *poFeature)

{
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;
    int bNeedComma = FALSE;

    int bEmptyInsert = FALSE;

    poDS->EndCopy();

    /* -------------------------------------------------------------------- */
    /*      Form the INSERT command.                                        */
    /* -------------------------------------------------------------------- */
    osCommand.Printf("INSERT INTO %s (", pszSqlTableName);

    for (int i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
    {
        OGRGeomFieldDefn *poGeomFieldDefn = poFeatureDefn->GetGeomFieldDefn(i);
        OGRGeometry *poGeom = poFeature->GetGeomFieldRef(i);
        if (poGeom == nullptr)
            continue;
        if (!bNeedComma)
            bNeedComma = TRUE;
        else
            osCommand += ", ";
        osCommand += OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef()) + " ";
    }

    /* Use case of ogr_pg_60 test */
    if (poFeature->GetFID() != OGRNullFID && pszFIDColumn != nullptr)
    {
        bNeedToUpdateSequence = true;

        if (bNeedComma)
            osCommand += ", ";

        osCommand = osCommand + OGRPGEscapeColumnName(pszFIDColumn) + " ";
        bNeedComma = TRUE;
    }
    else
    {
        UpdateSequenceIfNeeded();
    }

    const int nFieldCount = poFeatureDefn->GetFieldCount();
    for (int i = 0; i < nFieldCount; i++)
    {
        if (iFIDAsRegularColumnIndex == i)
            continue;
        if (!poFeature->IsFieldSet(i))
            continue;
        if (poFeature->GetDefnRef()->GetFieldDefn(i)->IsGenerated())
            continue;

        if (!bNeedComma)
            bNeedComma = TRUE;
        else
            osCommand += ", ";

        osCommand +=
            OGRPGEscapeColumnName(poFeatureDefn->GetFieldDefn(i)->GetNameRef());
    }

    if (!bNeedComma)
        bEmptyInsert = TRUE;

    osCommand += ") VALUES (";

    /* Set the geometry */
    bNeedComma = FALSE;
    for (int i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
    {
        const OGRPGGeomFieldDefn *poGeomFieldDefn =
            poFeatureDefn->GetGeomFieldDefn(i);
        OGRGeometry *poGeom = poFeature->GetGeomFieldRef(i);
        if (poGeom == nullptr)
            continue;
        if (bNeedComma)
            osCommand += ", ";
        else
            bNeedComma = TRUE;

        if (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOGRAPHY ||
            poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOMETRY)
        {
            CheckGeomTypeCompatibility(i, poGeom);

            poGeom->closeRings();
            poGeom->set3D(poGeomFieldDefn->GeometryTypeFlags &
                          OGRGeometry::OGR_G_3D);
            poGeom->setMeasured(poGeomFieldDefn->GeometryTypeFlags &
                                OGRGeometry::OGR_G_MEASURED);

            int nSRSId = poGeomFieldDefn->nSRSId;

            char *pszHexEWKB = OGRGeometryToHexEWKB(
                poGeom, nSRSId, poDS->sPostGISVersion.nMajor,
                poDS->sPostGISVersion.nMinor);
            if (!pszHexEWKB || pszHexEWKB[0] == 0)
            {
                CPLFree(pszHexEWKB);
                return OGRERR_FAILURE;
            }
            osCommand += '\'';
            try
            {
                osCommand += pszHexEWKB;
            }
            catch (const std::bad_alloc &)
            {
                CPLError(CE_Failure, CPLE_OutOfMemory,
                         "Out of memory: too large geometry");
                CPLFree(pszHexEWKB);
                return OGRERR_FAILURE;
            }
            osCommand += "'::";
            if (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOGRAPHY)
                osCommand += "GEOGRAPHY";
            else
                osCommand += "GEOMETRY";
            CPLFree(pszHexEWKB);
        }
        else if (!bWkbAsOid)
        {
            char *pszBytea =
                GeometryToBYTEA(poGeom, poDS->sPostGISVersion.nMajor,
                                poDS->sPostGISVersion.nMinor);

            if (!pszBytea)
            {
                return OGRERR_FAILURE;
            }
            osCommand += "E'";
            try
            {
                osCommand += pszBytea;
            }
            catch (const std::bad_alloc &)
            {
                CPLError(CE_Failure, CPLE_OutOfMemory,
                         "Out of memory: too large geometry");
                CPLFree(pszBytea);
                return OGRERR_FAILURE;
            }
            osCommand += '\'';
            CPLFree(pszBytea);
        }
        else if (poGeomFieldDefn->ePostgisType ==
                 GEOM_TYPE_WKB /* && bWkbAsOid */)
        {
            Oid oid = GeometryToOID(poGeom);

            if (oid != 0)
            {
                osCommand += CPLString().Printf("'%d' ", oid);
            }
            else
                osCommand += "''";
        }
    }

    if (poFeature->GetFID() != OGRNullFID && pszFIDColumn != nullptr)
    {
        if (bNeedComma)
            osCommand += ", ";
        osCommand += CPLString().Printf(CPL_FRMT_GIB " ", poFeature->GetFID());
        bNeedComma = TRUE;
    }

    for (int i = 0; i < nFieldCount; i++)
    {
        if (iFIDAsRegularColumnIndex == i)
            continue;
        if (!poFeature->IsFieldSet(i))
            continue;
        if (poFeature->GetDefnRef()->GetFieldDefn(i)->IsGenerated())
            continue;

        if (bNeedComma)
            osCommand += ", ";
        else
            bNeedComma = TRUE;

        OGRPGCommonAppendFieldValue(osCommand, poFeature, i, OGRPGEscapeString,
                                    hPGConn);
    }

    osCommand += ")";

    if (bEmptyInsert)
        osCommand.Printf("INSERT INTO %s DEFAULT VALUES", pszSqlTableName);

    int bReturnRequested = FALSE;
    /* We only get the FID, but we also could add the unset fields to get */
    /* the default values */
    if (bRetrieveFID && pszFIDColumn != nullptr &&
        poFeature->GetFID() == OGRNullFID)
    {
        if (bSkipConflicts)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "fid retrieval and skipping conflicts are not supported "
                     "at the same time.");
            return OGRERR_FAILURE;
        }
        bReturnRequested = TRUE;
        osCommand += " RETURNING ";
        osCommand += OGRPGEscapeColumnName(pszFIDColumn);
    }
    else if (bSkipConflicts)
        osCommand += " ON CONFLICT DO NOTHING";

    /* -------------------------------------------------------------------- */
    /*      Execute the insert.                                             */
    /* -------------------------------------------------------------------- */
    PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
    if (bReturnRequested && PQresultStatus(hResult) == PGRES_TUPLES_OK &&
        PQntuples(hResult) == 1 && PQnfields(hResult) == 1)
    {
        const char *pszFID = PQgetvalue(hResult, 0, 0);
        poFeature->SetFID(CPLAtoGIntBig(pszFID));
    }
    else if (bReturnRequested || PQresultStatus(hResult) != PGRES_COMMAND_OK)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "INSERT command for new feature failed.\n%s\nCommand: %s",
                 PQerrorMessage(hPGConn), osCommand.substr(0, 1024).c_str());

        if (!bHasWarnedAlreadySetFID && poFeature->GetFID() != OGRNullFID &&
            pszFIDColumn != nullptr)
        {
            bHasWarnedAlreadySetFID = TRUE;
            CPLError(CE_Warning, CPLE_AppDefined,
                     "You've inserted feature with an already set FID and "
                     "that's perhaps the reason for the failure. "
                     "If so, this can happen if you reuse the same feature "
                     "object for sequential insertions. "
                     "Indeed, since GDAL 1.8.0, the FID of an inserted feature "
                     "is got from the server, so it is not a good idea"
                     "to reuse it afterwards... All in all, try unsetting the "
                     "FID with SetFID(-1) before calling CreateFeature()");
        }

        OGRPGClearResult(hResult);

        return OGRERR_FAILURE;
    }

    OGRPGClearResult(hResult);

    return OGRERR_NONE;
}

/************************************************************************/
/*                        CreateFeatureViaCopy()                        */
/************************************************************************/

OGRErr OGRPGTableLayer::CreateFeatureViaCopy(OGRFeature *poFeature)
{
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;

    /* Tell the datasource we are now planning to copy data */
    poDS->StartCopy(this);

    /* First process geometry */
    for (int i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
    {
        const OGRPGGeomFieldDefn *poGeomFieldDefn =
            poFeatureDefn->GetGeomFieldDefn(i);
        OGRGeometry *poGeom = poFeature->GetGeomFieldRef(i);

        char *pszGeom = nullptr;
        if (nullptr != poGeom)
        {
            CheckGeomTypeCompatibility(i, poGeom);

            poGeom->closeRings();
            poGeom->set3D(poGeomFieldDefn->GeometryTypeFlags &
                          OGRGeometry::OGR_G_3D);
            poGeom->setMeasured(poGeomFieldDefn->GeometryTypeFlags &
                                OGRGeometry::OGR_G_MEASURED);

            if (poGeomFieldDefn->ePostgisType == GEOM_TYPE_WKB)
                pszGeom = GeometryToBYTEA(poGeom, poDS->sPostGISVersion.nMajor,
                                          poDS->sPostGISVersion.nMinor);
            else
                pszGeom = OGRGeometryToHexEWKB(poGeom, poGeomFieldDefn->nSRSId,
                                               poDS->sPostGISVersion.nMajor,
                                               poDS->sPostGISVersion.nMinor);
            if (!pszGeom || pszGeom[0] == 0)
            {
                CPLFree(pszGeom);
                return OGRERR_FAILURE;
            }
        }

        if (!osCommand.empty())
            osCommand += "\t";

        if (pszGeom)
        {
            try
            {
                osCommand += pszGeom;
            }
            catch (const std::bad_alloc &)
            {
                CPLError(CE_Failure, CPLE_OutOfMemory,
                         "Out of memory: too large geometry");
                CPLFree(pszGeom);
                return OGRERR_FAILURE;
            }
            CPLFree(pszGeom);
        }
        else
        {
            osCommand += "\\N";
        }
    }

    std::vector<bool> abFieldsToInclude(poFeature->GetFieldCount(), true);
    for (size_t i = 0; i < abFieldsToInclude.size(); i++)
        abFieldsToInclude[i] = !poFeature->GetDefnRef()
                                    ->GetFieldDefn(static_cast<int>(i))
                                    ->IsGenerated();

    if (bFIDColumnInCopyFields)
    {
        OGRPGCommonAppendCopyFID(osCommand, poFeature);
    }
    OGRPGCommonAppendCopyRegularFields(osCommand, poFeature, pszFIDColumn,
                                       abFieldsToInclude, OGRPGEscapeString,
                                       hPGConn);

    /* Add end of line marker */
    osCommand += "\n";

    // PostgreSQL doesn't provide very helpful reporting of invalid UTF-8
    // content in COPY mode.
    if (poDS->IsUTF8ClientEncoding() &&
        !CPLIsUTF8(osCommand.c_str(), static_cast<int>(osCommand.size())))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Non UTF-8 content found when writing feature " CPL_FRMT_GIB
                 " of layer %s: %s",
                 poFeature->GetFID(), poFeatureDefn->GetName(),
                 osCommand.c_str());
        return OGRERR_FAILURE;
    }

    /* ------------------------------------------------------------ */
    /*      Execute the copy.                                       */
    /* ------------------------------------------------------------ */

    OGRErr result = OGRERR_NONE;

    int copyResult = PQputCopyData(hPGConn, osCommand.c_str(),
                                   static_cast<int>(osCommand.size()));
#ifdef DEBUG_VERBOSE
    CPLDebug("PG", "PQputCopyData(%s)", osCommand.c_str());
#endif

    switch (copyResult)
    {
        case 0:
            CPLError(CE_Failure, CPLE_AppDefined, "Writing COPY data blocked.");
            result = OGRERR_FAILURE;
            break;
        case -1:
            CPLError(CE_Failure, CPLE_AppDefined, "%s",
                     PQerrorMessage(hPGConn));
            result = OGRERR_FAILURE;
            break;
    }

    return result;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRPGTableLayer::TestCapability(const char *pszCap)

{
    if (bUpdateAccess)
    {
        if (EQUAL(pszCap, OLCSequentialWrite) ||
            EQUAL(pszCap, OLCCreateField) ||
            EQUAL(pszCap, OLCCreateGeomField) ||
            EQUAL(pszCap, OLCDeleteField) || EQUAL(pszCap, OLCAlterFieldDefn) ||
            EQUAL(pszCap, OLCAlterGeomFieldDefn) || EQUAL(pszCap, OLCRename))
            return TRUE;

        else if (EQUAL(pszCap, OLCRandomWrite) ||
                 EQUAL(pszCap, OLCUpdateFeature) ||
                 EQUAL(pszCap, OLCDeleteFeature))
        {
            GetLayerDefn()->GetFieldCount();
            return pszFIDColumn != nullptr;
        }
    }

    if (EQUAL(pszCap, OLCRandomRead))
    {
        GetLayerDefn()->GetFieldCount();
        return pszFIDColumn != nullptr;
    }

    else if (EQUAL(pszCap, OLCFastFeatureCount) ||
             EQUAL(pszCap, OLCFastSetNextByIndex))
    {
        if (m_poFilterGeom == nullptr)
            return TRUE;
        OGRPGGeomFieldDefn *poGeomFieldDefn = nullptr;
        if (poFeatureDefn->GetGeomFieldCount() > 0)
            poGeomFieldDefn =
                poFeatureDefn->GetGeomFieldDefn(m_iGeomFieldFilter);
        return poGeomFieldDefn == nullptr ||
               (poDS->sPostGISVersion.nMajor >= 0 &&
                (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOMETRY ||
                 poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOGRAPHY));
    }

    else if (EQUAL(pszCap, OLCFastSpatialFilter))
    {
        OGRPGGeomFieldDefn *poGeomFieldDefn = nullptr;
        if (poFeatureDefn->GetGeomFieldCount() > 0)
            poGeomFieldDefn =
                poFeatureDefn->GetGeomFieldDefn(m_iGeomFieldFilter);
        return poGeomFieldDefn == nullptr ||
               (poDS->sPostGISVersion.nMajor >= 0 &&
                (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOMETRY ||
                 poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOGRAPHY));
    }

    else if (EQUAL(pszCap, OLCTransactions))
        return TRUE;

    else if (EQUAL(pszCap, OLCFastGetExtent) ||
             EQUAL(pszCap, OLCFastGetExtent3D))
    {
        OGRPGGeomFieldDefn *poGeomFieldDefn = nullptr;
        if (poFeatureDefn->GetGeomFieldCount() > 0)
            poGeomFieldDefn = poFeatureDefn->GetGeomFieldDefn(0);
        return poGeomFieldDefn != nullptr &&
               poDS->sPostGISVersion.nMajor >= 0 &&
               poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOMETRY;
    }

    else if (EQUAL(pszCap, OLCStringsAsUTF8))
        return TRUE;

    else if (EQUAL(pszCap, OLCCurveGeometries))
        return TRUE;

    else if (EQUAL(pszCap, OLCMeasuredGeometries))
        return TRUE;

    else if (EQUAL(pszCap, OLCZGeometries))
        return TRUE;

    else
        return FALSE;
}

/************************************************************************/
/*                            CreateField()                             */
/************************************************************************/

OGRErr OGRPGTableLayer::CreateField(const OGRFieldDefn *poFieldIn,
                                    int bApproxOK)

{
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;
    CPLString osFieldType;
    OGRFieldDefn oField(poFieldIn);

    GetLayerDefn()->GetFieldCount();

    if (!bUpdateAccess)
    {
        CPLError(CE_Failure, CPLE_NotSupported, UNSUPPORTED_OP_READ_ONLY,
                 "CreateField");
        return OGRERR_FAILURE;
    }

    if (pszFIDColumn != nullptr && EQUAL(oField.GetNameRef(), pszFIDColumn) &&
        oField.GetType() != OFTInteger && oField.GetType() != OFTInteger64)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Wrong field type for %s",
                 oField.GetNameRef());
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Do we want to "launder" the column names into Postgres          */
    /*      friendly format?                                                */
    /* -------------------------------------------------------------------- */
    if (bLaunderColumnNames)
    {
        char *pszSafeName =
            OGRPGCommonLaunderName(oField.GetNameRef(), "PG", m_bUTF8ToASCII);

        oField.SetName(pszSafeName);
        CPLFree(pszSafeName);

        if (EQUAL(oField.GetNameRef(), "oid"))
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Renaming field 'oid' to 'oid_' to avoid conflict with "
                     "internal oid field.");
            oField.SetName("oid_");
        }
    }

    const char *pszOverrideType =
        CSLFetchNameValue(papszOverrideColumnTypes, oField.GetNameRef());
    if (pszOverrideType != nullptr)
        osFieldType = pszOverrideType;
    else
    {
        osFieldType = OGRPGCommonLayerGetType(
            oField, CPL_TO_BOOL(bPreservePrecision), CPL_TO_BOOL(bApproxOK));
        if (osFieldType.empty())
            return OGRERR_FAILURE;
    }

    CPLString osConstraints;
    if (!oField.IsNullable())
        osConstraints += " NOT NULL";
    if (oField.IsUnique())
        osConstraints += " UNIQUE";
    if (oField.GetDefault() != nullptr && !oField.IsDefaultDriverSpecific())
    {
        osConstraints += " DEFAULT ";
        osConstraints += OGRPGCommonLayerGetPGDefault(&oField);
    }

    std::string osCommentON;
    if (!oField.GetComment().empty())
    {
        osCommentON = "COMMENT ON COLUMN ";
        osCommentON += pszSqlTableName;
        osCommentON += '.';
        osCommentON += OGRPGEscapeColumnName(oField.GetNameRef());
        osCommentON += " IS ";
        osCommentON += OGRPGEscapeString(hPGConn, oField.GetComment().c_str());
    }

    /* -------------------------------------------------------------------- */
    /*      Create the new field.                                           */
    /* -------------------------------------------------------------------- */
    if (bDeferredCreation)
    {
        if (!(pszFIDColumn != nullptr &&
              EQUAL(pszFIDColumn, oField.GetNameRef())))
        {
            osCreateTable += ", ";
            osCreateTable += OGRPGEscapeColumnName(oField.GetNameRef());
            osCreateTable += " ";
            osCreateTable += osFieldType;
            osCreateTable += osConstraints;

            if (!osCommentON.empty())
                m_aosDeferredCommentOnColumns.push_back(osCommentON);
        }
    }
    else
    {
        poDS->EndCopy();

        osCommand.Printf("ALTER TABLE %s ADD COLUMN %s %s", pszSqlTableName,
                         OGRPGEscapeColumnName(oField.GetNameRef()).c_str(),
                         osFieldType.c_str());
        osCommand += osConstraints;

        PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
        if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s\n%s", osCommand.c_str(),
                     PQerrorMessage(hPGConn));

            OGRPGClearResult(hResult);

            return OGRERR_FAILURE;
        }

        OGRPGClearResult(hResult);

        if (!osCommentON.empty())
        {
            hResult = OGRPG_PQexec(hPGConn, osCommentON.c_str());
            OGRPGClearResult(hResult);
        }
    }

    whileUnsealing(poFeatureDefn)->AddFieldDefn(&oField);

    if (pszFIDColumn != nullptr && EQUAL(oField.GetNameRef(), pszFIDColumn))
    {
        iFIDAsRegularColumnIndex = poFeatureDefn->GetFieldCount() - 1;
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                        RunAddGeometryColumn()                        */
/************************************************************************/

OGRErr
OGRPGTableLayer::RunAddGeometryColumn(const OGRPGGeomFieldDefn *poGeomField)
{
    PGconn *hPGConn = poDS->GetPGConn();

    const char *pszGeometryType = OGRToOGCGeomType(poGeomField->GetType());
    const char *suffix = "";
    int dim = 2;
    if ((poGeomField->GeometryTypeFlags & OGRGeometry::OGR_G_3D) &&
        (poGeomField->GeometryTypeFlags & OGRGeometry::OGR_G_MEASURED))
        dim = 4;
    else if ((poGeomField->GeometryTypeFlags & OGRGeometry::OGR_G_MEASURED))
    {
        if (!(wkbFlatten(poGeomField->GetType()) == wkbUnknown))
            suffix = "M";
        dim = 3;
    }
    else if (poGeomField->GeometryTypeFlags & OGRGeometry::OGR_G_3D)
        dim = 3;

    CPLString osCommand;
    osCommand.Printf(
        "SELECT AddGeometryColumn(%s,%s,%s,%d,'%s%s',%d)",
        OGRPGEscapeString(hPGConn, pszSchemaName).c_str(),
        OGRPGEscapeString(hPGConn, pszTableName).c_str(),
        OGRPGEscapeString(hPGConn, poGeomField->GetNameRef()).c_str(),
        poGeomField->nSRSId, pszGeometryType, suffix, dim);

    PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());

    if (!hResult || PQresultStatus(hResult) != PGRES_TUPLES_OK)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "AddGeometryColumn failed for layer %s.", GetName());

        OGRPGClearResult(hResult);

        return OGRERR_FAILURE;
    }

    OGRPGClearResult(hResult);

    if (!poGeomField->IsNullable())
    {
        osCommand.Printf(
            "ALTER TABLE %s ALTER COLUMN %s SET NOT NULL", pszSqlTableName,
            OGRPGEscapeColumnName(poGeomField->GetNameRef()).c_str());

        hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());
        OGRPGClearResult(hResult);
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                        RunCreateSpatialIndex()                       */
/************************************************************************/

OGRErr
OGRPGTableLayer::RunCreateSpatialIndex(const OGRPGGeomFieldDefn *poGeomField,
                                       int nIdx)
{
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;

    const std::string osIndexName(OGRPGCommonGenerateSpatialIndexName(
        pszTableName, poGeomField->GetNameRef(), nIdx));

    osCommand.Printf("CREATE INDEX %s ON %s USING %s (%s)",
                     OGRPGEscapeColumnName(osIndexName.c_str()).c_str(),
                     pszSqlTableName, osSpatialIndexType.c_str(),
                     OGRPGEscapeColumnName(poGeomField->GetNameRef()).c_str());

    PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());

    if (!hResult || PQresultStatus(hResult) != PGRES_COMMAND_OK)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "CREATE INDEX failed for layer %s.", GetName());

        OGRPGClearResult(hResult);

        return OGRERR_FAILURE;
    }

    OGRPGClearResult(hResult);

    return OGRERR_NONE;
}

/************************************************************************/
/*                           CreateGeomField()                          */
/************************************************************************/

OGRErr OGRPGTableLayer::CreateGeomField(const OGRGeomFieldDefn *poGeomFieldIn,
                                        CPL_UNUSED int bApproxOK)
{
    OGRwkbGeometryType eType = poGeomFieldIn->GetType();
    if (eType == wkbNone)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot create geometry field of type wkbNone");
        return OGRERR_FAILURE;
    }

    // Check if GEOMETRY_NAME layer creation option was set, but no initial
    // column was created in ICreateLayer()
    CPLString osGeomFieldName = (m_osFirstGeometryFieldName.size())
                                    ? m_osFirstGeometryFieldName
                                    : CPLString(poGeomFieldIn->GetNameRef());
    m_osFirstGeometryFieldName = "";  // reset for potential next geom columns

    auto poGeomField =
        std::make_unique<OGRPGGeomFieldDefn>(this, osGeomFieldName);
    if (EQUAL(poGeomField->GetNameRef(), ""))
    {
        if (poFeatureDefn->GetGeomFieldCount() == 0)
            poGeomField->SetName(EQUAL(m_osLCOGeomType.c_str(), "geography")
                                     ? "the_geog"
                                     : "wkb_geometry");
        else
            poGeomField->SetName(CPLSPrintf(
                "wkb_geometry%d", poFeatureDefn->GetGeomFieldCount() + 1));
    }
    const auto poSRSIn = poGeomFieldIn->GetSpatialRef();
    if (poSRSIn)
    {
        auto l_poSRS = poSRSIn->Clone();
        l_poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        poGeomField->SetSpatialRef(l_poSRS);
        l_poSRS->Release();
    }
    /* -------------------------------------------------------------------- */
    /*      Do we want to "launder" the column names into Postgres          */
    /*      friendly format?                                                */
    /* -------------------------------------------------------------------- */
    if (bLaunderColumnNames)
    {
        char *pszSafeName = OGRPGCommonLaunderName(poGeomField->GetNameRef(),
                                                   "PG", m_bUTF8ToASCII);

        poGeomField->SetName(pszSafeName);
        CPLFree(pszSafeName);
    }

    const OGRSpatialReference *poSRS = poGeomField->GetSpatialRef();
    int nSRSId = poDS->GetUndefinedSRID();
    if (nForcedSRSId != UNDETERMINED_SRID)
        nSRSId = nForcedSRSId;
    else if (poSRS != nullptr)
        nSRSId = poDS->FetchSRSId(poSRS);

    int GeometryTypeFlags = 0;
    if (OGR_GT_HasZ(eType))
        GeometryTypeFlags |= OGRGeometry::OGR_G_3D;
    if (OGR_GT_HasM(eType))
        GeometryTypeFlags |= OGRGeometry::OGR_G_MEASURED;
    if (nForcedGeometryTypeFlags >= 0)
    {
        GeometryTypeFlags = nForcedGeometryTypeFlags;
        eType =
            OGR_GT_SetModifier(eType, GeometryTypeFlags & OGRGeometry::OGR_G_3D,
                               GeometryTypeFlags & OGRGeometry::OGR_G_MEASURED);
    }
    poGeomField->SetType(eType);
    poGeomField->SetNullable(poGeomFieldIn->IsNullable());
    poGeomField->nSRSId = nSRSId;
    poGeomField->GeometryTypeFlags = GeometryTypeFlags;
    poGeomField->ePostgisType = EQUAL(m_osLCOGeomType.c_str(), "geography")
                                    ? GEOM_TYPE_GEOGRAPHY
                                    : GEOM_TYPE_GEOMETRY;

    /* -------------------------------------------------------------------- */
    /*      Create the new field.                                           */
    /* -------------------------------------------------------------------- */
    if (!bDeferredCreation)
    {
        poDS->EndCopy();

        if (RunAddGeometryColumn(poGeomField.get()) != OGRERR_NONE)
        {
            return OGRERR_FAILURE;
        }

        if (bCreateSpatialIndexFlag)
        {
            if (RunCreateSpatialIndex(poGeomField.get(), 0) != OGRERR_NONE)
            {
                return OGRERR_FAILURE;
            }
        }
    }

    whileUnsealing(poFeatureDefn)->AddGeomFieldDefn(std::move(poGeomField));

    return OGRERR_NONE;
}

/************************************************************************/
/*                            DeleteField()                             */
/************************************************************************/

OGRErr OGRPGTableLayer::DeleteField(int iField)
{
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;

    GetLayerDefn()->GetFieldCount();

    if (!bUpdateAccess)
    {
        CPLError(CE_Failure, CPLE_NotSupported, UNSUPPORTED_OP_READ_ONLY,
                 "DeleteField");
        return OGRERR_FAILURE;
    }

    if (iField < 0 || iField >= poFeatureDefn->GetFieldCount())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Invalid field index");
        return OGRERR_FAILURE;
    }

    if (bDeferredCreation && RunDeferredCreationIfNecessary() != OGRERR_NONE)
        return OGRERR_FAILURE;
    poDS->EndCopy();

    osCommand.Printf(
        "ALTER TABLE %s DROP COLUMN %s", pszSqlTableName,
        OGRPGEscapeColumnName(poFeatureDefn->GetFieldDefn(iField)->GetNameRef())
            .c_str());
    PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
    if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s\n%s", osCommand.c_str(),
                 PQerrorMessage(hPGConn));

        OGRPGClearResult(hResult);

        return OGRERR_FAILURE;
    }

    OGRPGClearResult(hResult);

    return whileUnsealing(poFeatureDefn)->DeleteFieldDefn(iField);
}

/************************************************************************/
/*                           AlterFieldDefn()                           */
/************************************************************************/

OGRErr OGRPGTableLayer::AlterFieldDefn(int iField, OGRFieldDefn *poNewFieldDefn,
                                       int nFlagsIn)
{
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;

    GetLayerDefn()->GetFieldCount();

    if (!bUpdateAccess)
    {
        CPLError(CE_Failure, CPLE_NotSupported, UNSUPPORTED_OP_READ_ONLY,
                 "AlterFieldDefn");
        return OGRERR_FAILURE;
    }

    if (iField < 0 || iField >= poFeatureDefn->GetFieldCount())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Invalid field index");
        return OGRERR_FAILURE;
    }

    if (bDeferredCreation && RunDeferredCreationIfNecessary() != OGRERR_NONE)
        return OGRERR_FAILURE;
    poDS->EndCopy();

    OGRFieldDefn *poFieldDefn = poFeatureDefn->GetFieldDefn(iField);
    auto oTemporaryUnsealer(poFieldDefn->GetTemporaryUnsealer());
    OGRFieldDefn oField(poNewFieldDefn);

    poDS->SoftStartTransaction();

    if (!(nFlagsIn & ALTER_TYPE_FLAG))
    {
        oField.SetSubType(OFSTNone);
        oField.SetType(poFieldDefn->GetType());
        oField.SetSubType(poFieldDefn->GetSubType());
    }

    if (!(nFlagsIn & ALTER_WIDTH_PRECISION_FLAG))
    {
        oField.SetWidth(poFieldDefn->GetWidth());
        oField.SetPrecision(poFieldDefn->GetPrecision());
    }

    if ((nFlagsIn & ALTER_TYPE_FLAG) || (nFlagsIn & ALTER_WIDTH_PRECISION_FLAG))
    {
        CPLString osFieldType = OGRPGCommonLayerGetType(
            oField, CPL_TO_BOOL(bPreservePrecision), true);
        if (osFieldType.empty())
        {
            poDS->SoftRollbackTransaction();

            return OGRERR_FAILURE;
        }

        osCommand.Printf(
            "ALTER TABLE %s ALTER COLUMN %s TYPE %s", pszSqlTableName,
            OGRPGEscapeColumnName(poFieldDefn->GetNameRef()).c_str(),
            osFieldType.c_str());

        PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
        if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s\n%s", osCommand.c_str(),
                     PQerrorMessage(hPGConn));

            OGRPGClearResult(hResult);

            poDS->SoftRollbackTransaction();

            return OGRERR_FAILURE;
        }
        OGRPGClearResult(hResult);
    }

    if ((nFlagsIn & ALTER_NULLABLE_FLAG) &&
        poFieldDefn->IsNullable() != poNewFieldDefn->IsNullable())
    {
        oField.SetNullable(poNewFieldDefn->IsNullable());

        if (poNewFieldDefn->IsNullable())
            osCommand.Printf(
                "ALTER TABLE %s ALTER COLUMN %s DROP NOT NULL", pszSqlTableName,
                OGRPGEscapeColumnName(poFieldDefn->GetNameRef()).c_str());
        else
            osCommand.Printf(
                "ALTER TABLE %s ALTER COLUMN %s SET NOT NULL", pszSqlTableName,
                OGRPGEscapeColumnName(poFieldDefn->GetNameRef()).c_str());

        PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
        if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s\n%s", osCommand.c_str(),
                     PQerrorMessage(hPGConn));

            OGRPGClearResult(hResult);

            poDS->SoftRollbackTransaction();

            return OGRERR_FAILURE;
        }
        OGRPGClearResult(hResult);
    }

    // Only supports adding a unique constraint
    if ((nFlagsIn & ALTER_UNIQUE_FLAG) && !poFieldDefn->IsUnique() &&
        poNewFieldDefn->IsUnique())
    {
        oField.SetUnique(poNewFieldDefn->IsUnique());

        osCommand.Printf(
            "ALTER TABLE %s ADD UNIQUE (%s)", pszSqlTableName,
            OGRPGEscapeColumnName(poFieldDefn->GetNameRef()).c_str());

        PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
        if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s\n%s", osCommand.c_str(),
                     PQerrorMessage(hPGConn));

            OGRPGClearResult(hResult);

            poDS->SoftRollbackTransaction();

            return OGRERR_FAILURE;
        }
        OGRPGClearResult(hResult);
    }
    else if ((nFlagsIn & ALTER_UNIQUE_FLAG) && poFieldDefn->IsUnique() &&
             !poNewFieldDefn->IsUnique())
    {
        oField.SetUnique(TRUE);
        CPLError(CE_Warning, CPLE_NotSupported,
                 "Dropping a UNIQUE constraint is not supported currently");
    }

    if ((nFlagsIn & ALTER_DEFAULT_FLAG) &&
        ((poFieldDefn->GetDefault() == nullptr &&
          poNewFieldDefn->GetDefault() != nullptr) ||
         (poFieldDefn->GetDefault() != nullptr &&
          poNewFieldDefn->GetDefault() == nullptr) ||
         (poFieldDefn->GetDefault() != nullptr &&
          poNewFieldDefn->GetDefault() != nullptr &&
          strcmp(poFieldDefn->GetDefault(), poNewFieldDefn->GetDefault()) !=
              0)))
    {
        oField.SetDefault(poNewFieldDefn->GetDefault());

        if (poNewFieldDefn->GetDefault() == nullptr)
            osCommand.Printf(
                "ALTER TABLE %s ALTER COLUMN %s DROP DEFAULT", pszSqlTableName,
                OGRPGEscapeColumnName(poFieldDefn->GetNameRef()).c_str());
        else
            osCommand.Printf(
                "ALTER TABLE %s ALTER COLUMN %s SET DEFAULT %s",
                pszSqlTableName,
                OGRPGEscapeColumnName(poFieldDefn->GetNameRef()).c_str(),
                OGRPGCommonLayerGetPGDefault(poNewFieldDefn).c_str());

        PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
        if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s\n%s", osCommand.c_str(),
                     PQerrorMessage(hPGConn));

            OGRPGClearResult(hResult);

            poDS->SoftRollbackTransaction();

            return OGRERR_FAILURE;
        }
        OGRPGClearResult(hResult);
    }

    if ((nFlagsIn & ALTER_COMMENT_FLAG) &&
        poFieldDefn->GetComment() != poNewFieldDefn->GetComment())
    {
        oField.SetComment(poNewFieldDefn->GetComment());

        if (!poNewFieldDefn->GetComment().empty())
        {
            osCommand.Printf(
                "COMMENT ON COLUMN %s.%s IS %s", pszSqlTableName,
                OGRPGEscapeColumnName(poFieldDefn->GetNameRef()).c_str(),
                OGRPGEscapeString(hPGConn, poNewFieldDefn->GetComment().c_str())
                    .c_str());
        }
        else
        {
            osCommand.Printf(
                "COMMENT ON COLUMN %s.%s IS NULL", pszSqlTableName,
                OGRPGEscapeColumnName(poFieldDefn->GetNameRef()).c_str());
        }

        PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
        if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s\n%s", osCommand.c_str(),
                     PQerrorMessage(hPGConn));

            OGRPGClearResult(hResult);

            poDS->SoftRollbackTransaction();

            return OGRERR_FAILURE;
        }
        OGRPGClearResult(hResult);
    }

    if ((nFlagsIn & ALTER_NAME_FLAG))
    {
        if (bLaunderColumnNames)
        {
            char *pszSafeName = OGRPGCommonLaunderName(oField.GetNameRef(),
                                                       "PG", m_bUTF8ToASCII);
            oField.SetName(pszSafeName);
            CPLFree(pszSafeName);
        }

        if (EQUAL(oField.GetNameRef(), "oid"))
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Renaming field 'oid' to 'oid_' to avoid conflict with "
                     "internal oid field.");
            oField.SetName("oid_");
        }

        if (strcmp(poFieldDefn->GetNameRef(), oField.GetNameRef()) != 0)
        {
            osCommand.Printf(
                "ALTER TABLE %s RENAME COLUMN %s TO %s", pszSqlTableName,
                OGRPGEscapeColumnName(poFieldDefn->GetNameRef()).c_str(),
                OGRPGEscapeColumnName(oField.GetNameRef()).c_str());
            PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
            if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
            {
                CPLError(CE_Failure, CPLE_AppDefined, "%s\n%s",
                         osCommand.c_str(), PQerrorMessage(hPGConn));

                OGRPGClearResult(hResult);

                poDS->SoftRollbackTransaction();

                return OGRERR_FAILURE;
            }
            OGRPGClearResult(hResult);
        }
    }

    poDS->SoftCommitTransaction();

    if (nFlagsIn & ALTER_NAME_FLAG)
        poFieldDefn->SetName(oField.GetNameRef());
    if (nFlagsIn & ALTER_TYPE_FLAG)
    {
        poFieldDefn->SetSubType(OFSTNone);
        poFieldDefn->SetType(oField.GetType());
        poFieldDefn->SetSubType(oField.GetSubType());
    }
    if (nFlagsIn & ALTER_WIDTH_PRECISION_FLAG)
    {
        poFieldDefn->SetWidth(oField.GetWidth());
        poFieldDefn->SetPrecision(oField.GetPrecision());
    }
    if (nFlagsIn & ALTER_NULLABLE_FLAG)
        poFieldDefn->SetNullable(oField.IsNullable());
    if (nFlagsIn & ALTER_DEFAULT_FLAG)
        poFieldDefn->SetDefault(oField.GetDefault());
    if (nFlagsIn & ALTER_UNIQUE_FLAG)
        poFieldDefn->SetUnique(oField.IsUnique());
    if (nFlagsIn & ALTER_COMMENT_FLAG)
        poFieldDefn->SetComment(oField.GetComment());

    return OGRERR_NONE;
}

/************************************************************************/
/*                         AlterGeomFieldDefn()                         */
/************************************************************************/

OGRErr
OGRPGTableLayer::AlterGeomFieldDefn(int iGeomFieldToAlter,
                                    const OGRGeomFieldDefn *poNewGeomFieldDefn,
                                    int nFlagsIn)
{
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;

    if (!bUpdateAccess)
    {
        CPLError(CE_Failure, CPLE_NotSupported, UNSUPPORTED_OP_READ_ONLY,
                 "AlterGeomFieldDefn");
        return OGRERR_FAILURE;
    }

    if (iGeomFieldToAlter < 0 ||
        iGeomFieldToAlter >= GetLayerDefn()->GetGeomFieldCount())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Invalid field index");
        return OGRERR_FAILURE;
    }

    if (bDeferredCreation && RunDeferredCreationIfNecessary() != OGRERR_NONE)
        return OGRERR_FAILURE;
    poDS->EndCopy();

    auto poGeomFieldDefn = cpl::down_cast<OGRPGGeomFieldDefn *>(
        poFeatureDefn->GetGeomFieldDefn(iGeomFieldToAlter));
    auto oTemporaryUnsealer(poGeomFieldDefn->GetTemporaryUnsealer());

    if (nFlagsIn & ALTER_GEOM_FIELD_DEFN_SRS_COORD_EPOCH_FLAG)
    {
        const auto poNewSRSRef = poNewGeomFieldDefn->GetSpatialRef();
        if (poNewSRSRef && poNewSRSRef->GetCoordinateEpoch() > 0)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Setting a coordinate epoch is not supported for "
                     "PostGIS");
            return OGRERR_FAILURE;
        }
    }

    const OGRGeomFieldDefn oGeomField(poNewGeomFieldDefn);
    poDS->SoftStartTransaction();

    int nGeomTypeFlags = poGeomFieldDefn->GeometryTypeFlags;

    if ((nFlagsIn & ALTER_GEOM_FIELD_DEFN_TYPE_FLAG) &&
        poGeomFieldDefn->GetType() != poNewGeomFieldDefn->GetType())
    {
        const char *pszGeometryType =
            OGRToOGCGeomType(poNewGeomFieldDefn->GetType());
        std::string osType;
        if (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOMETRY)
            osType += "geometry(";
        else
            osType += "geography(";
        osType += pszGeometryType;
        nGeomTypeFlags = 0;
        if (OGR_GT_HasZ(poNewGeomFieldDefn->GetType()))
            nGeomTypeFlags |= OGRGeometry::OGR_G_3D;
        if (OGR_GT_HasM(poNewGeomFieldDefn->GetType()))
            nGeomTypeFlags |= OGRGeometry::OGR_G_MEASURED;
        if (nGeomTypeFlags & OGRGeometry::OGR_G_3D)
            osType += "Z";
        else if (nGeomTypeFlags & OGRGeometry::OGR_G_MEASURED)
            osType += "M";
        if (poGeomFieldDefn->nSRSId > 0)
            osType += CPLSPrintf(",%d", poGeomFieldDefn->nSRSId);
        osType += ")";

        osCommand.Printf(
            "ALTER TABLE %s ALTER COLUMN %s TYPE %s", pszSqlTableName,
            OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef()).c_str(),
            osType.c_str());

        PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
        if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s\n%s", osCommand.c_str(),
                     PQerrorMessage(hPGConn));

            OGRPGClearResult(hResult);

            poDS->SoftRollbackTransaction();

            return OGRERR_FAILURE;
        }
        OGRPGClearResult(hResult);
    }

    const auto poOldSRS = poGeomFieldDefn->GetSpatialRef();
    int nSRID = poGeomFieldDefn->nSRSId;

    if ((nFlagsIn & ALTER_GEOM_FIELD_DEFN_SRS_FLAG))
    {
        const auto poNewSRS = poNewGeomFieldDefn->GetSpatialRef();
        const char *const apszOptions[] = {
            "IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES", nullptr};
        if ((poOldSRS == nullptr && poNewSRS != nullptr) ||
            (poOldSRS != nullptr && poNewSRS == nullptr) ||
            (poOldSRS != nullptr && poNewSRS != nullptr &&
             !poOldSRS->IsSame(poNewSRS, apszOptions)))
        {
            if (poNewSRS)
                nSRID = poDS->FetchSRSId(poNewSRS);
            else
                nSRID = 0;

            osCommand.Printf(
                "SELECT UpdateGeometrySRID(%s,%s,%s,%d)",
                OGRPGEscapeString(hPGConn, pszSchemaName).c_str(),
                OGRPGEscapeString(hPGConn, pszTableName).c_str(),
                OGRPGEscapeString(hPGConn, poGeomFieldDefn->GetNameRef())
                    .c_str(),
                nSRID);

            PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
            if (PQresultStatus(hResult) != PGRES_TUPLES_OK)
            {
                CPLError(CE_Failure, CPLE_AppDefined, "%s\n%s",
                         osCommand.c_str(), PQerrorMessage(hPGConn));

                OGRPGClearResult(hResult);

                poDS->SoftRollbackTransaction();

                return OGRERR_FAILURE;
            }
            OGRPGClearResult(hResult);
        }
    }

    if ((nFlagsIn & ALTER_GEOM_FIELD_DEFN_NULLABLE_FLAG) &&
        poGeomFieldDefn->IsNullable() != poNewGeomFieldDefn->IsNullable())
    {
        if (poNewGeomFieldDefn->IsNullable())
            osCommand.Printf(
                "ALTER TABLE %s ALTER COLUMN %s DROP NOT NULL", pszSqlTableName,
                OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef()).c_str());
        else
            osCommand.Printf(
                "ALTER TABLE %s ALTER COLUMN %s SET NOT NULL", pszSqlTableName,
                OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef()).c_str());

        PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
        if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s\n%s", osCommand.c_str(),
                     PQerrorMessage(hPGConn));

            OGRPGClearResult(hResult);

            poDS->SoftRollbackTransaction();

            return OGRERR_FAILURE;
        }
        OGRPGClearResult(hResult);
    }

    if ((nFlagsIn & ALTER_GEOM_FIELD_DEFN_NAME_FLAG) &&
        strcmp(poGeomFieldDefn->GetNameRef(),
               poNewGeomFieldDefn->GetNameRef()) != 0)
    {
        osCommand.Printf(
            "ALTER TABLE %s RENAME COLUMN %s TO %s", pszSqlTableName,
            OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef()).c_str(),
            OGRPGEscapeColumnName(oGeomField.GetNameRef()).c_str());
        PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
        if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s\n%s", osCommand.c_str(),
                     PQerrorMessage(hPGConn));

            OGRPGClearResult(hResult);

            poDS->SoftRollbackTransaction();

            return OGRERR_FAILURE;
        }
        OGRPGClearResult(hResult);
    }

    poDS->SoftCommitTransaction();

    if (nFlagsIn & ALTER_GEOM_FIELD_DEFN_NAME_FLAG)
        poGeomFieldDefn->SetName(oGeomField.GetNameRef());
    if (nFlagsIn & ALTER_GEOM_FIELD_DEFN_TYPE_FLAG)
    {
        poGeomFieldDefn->GeometryTypeFlags = nGeomTypeFlags;
        poGeomFieldDefn->SetType(oGeomField.GetType());
    }
    if (nFlagsIn & ALTER_GEOM_FIELD_DEFN_NULLABLE_FLAG)
        poGeomFieldDefn->SetNullable(oGeomField.IsNullable());
    if (nFlagsIn & ALTER_GEOM_FIELD_DEFN_SRS_FLAG)
    {
        const auto poSRSRef = oGeomField.GetSpatialRef();
        if (poSRSRef)
        {
            auto poSRSNew = poSRSRef->Clone();
            poGeomFieldDefn->SetSpatialRef(poSRSNew);
            poSRSNew->Release();
        }
        else
        {
            poGeomFieldDefn->SetSpatialRef(nullptr);
        }
        poGeomFieldDefn->nSRSId = nSRID;
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                             GetFeature()                             */
/************************************************************************/

OGRFeature *OGRPGTableLayer::GetFeature(GIntBig nFeatureId)

{
    GetLayerDefn()->GetFieldCount();

    if (pszFIDColumn == nullptr)
        return OGRLayer::GetFeature(nFeatureId);

    /* -------------------------------------------------------------------- */
    /*      Issue query for a single record.                                */
    /* -------------------------------------------------------------------- */
    OGRFeature *poFeature = nullptr;
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osFieldList = BuildFields();
    CPLString osCommand;

    poDS->EndCopy();
    poDS->SoftStartTransaction();

    osCommand.Printf("DECLARE getfeaturecursor %s for "
                     "SELECT %s FROM %s WHERE %s = " CPL_FRMT_GIB,
                     (poDS->bUseBinaryCursor) ? "BINARY CURSOR" : "CURSOR",
                     osFieldList.c_str(), pszSqlTableName,
                     OGRPGEscapeColumnName(pszFIDColumn).c_str(), nFeatureId);

    PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());

    if (hResult && PQresultStatus(hResult) == PGRES_COMMAND_OK)
    {
        OGRPGClearResult(hResult);

        hResult = OGRPG_PQexec(hPGConn, "FETCH ALL in getfeaturecursor");

        if (hResult && PQresultStatus(hResult) == PGRES_TUPLES_OK)
        {
            int nRows = PQntuples(hResult);
            if (nRows > 0)
            {
                int *panTempMapFieldNameToIndex = nullptr;
                int *panTempMapFieldNameToGeomIndex = nullptr;
                CreateMapFromFieldNameToIndex(hResult, poFeatureDefn,
                                              panTempMapFieldNameToIndex,
                                              panTempMapFieldNameToGeomIndex);
                poFeature = RecordToFeature(hResult, panTempMapFieldNameToIndex,
                                            panTempMapFieldNameToGeomIndex, 0);
                CPLFree(panTempMapFieldNameToIndex);
                CPLFree(panTempMapFieldNameToGeomIndex);
                if (poFeature && iFIDAsRegularColumnIndex >= 0)
                {
                    poFeature->SetField(iFIDAsRegularColumnIndex,
                                        poFeature->GetFID());
                }

                if (nRows > 1)
                {
                    CPLError(
                        CE_Warning, CPLE_AppDefined,
                        "%d rows in response to the WHERE %s = " CPL_FRMT_GIB
                        " clause !",
                        nRows, pszFIDColumn, nFeatureId);
                }
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Attempt to read feature with unknown feature id "
                         "(" CPL_FRMT_GIB ").",
                         nFeatureId);
            }
        }
    }
    else if (hResult && PQresultStatus(hResult) == PGRES_FATAL_ERROR)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s",
                 PQresultErrorMessage(hResult));
    }

    /* -------------------------------------------------------------------- */
    /*      Cleanup                                                         */
    /* -------------------------------------------------------------------- */
    OGRPGClearResult(hResult);

    hResult = OGRPG_PQexec(hPGConn, "CLOSE getfeaturecursor");
    OGRPGClearResult(hResult);

    poDS->SoftCommitTransaction();

    return poFeature;
}

/************************************************************************/
/*                          GetFeatureCount()                           */
/************************************************************************/

GIntBig OGRPGTableLayer::GetFeatureCount(int bForce)

{
    if (bDeferredCreation && RunDeferredCreationIfNecessary() != OGRERR_NONE)
        return 0;
    poDS->EndCopy();

    if (TestCapability(OLCFastFeatureCount) == FALSE)
        return OGRPGLayer::GetFeatureCount(bForce);

    /* -------------------------------------------------------------------- */
    /*      In theory it might be wise to cache this result, but it         */
    /*      won't be trivial to work out the lifetime of the value.         */
    /*      After all someone else could be adding records from another     */
    /*      application when working against a database.                    */
    /* -------------------------------------------------------------------- */
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;
    GIntBig nCount = 0;

    osCommand.Printf("SELECT count(*) FROM %s %s", pszSqlTableName,
                     osWHERE.c_str());

    PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
    if (hResult != nullptr && PQresultStatus(hResult) == PGRES_TUPLES_OK)
        nCount = CPLAtoGIntBig(PQgetvalue(hResult, 0, 0));
    else
        CPLDebug("PG", "%s; failed.", osCommand.c_str());
    OGRPGClearResult(hResult);

    return nCount;
}

/************************************************************************/
/*                             ResolveSRID()                            */
/************************************************************************/

void OGRPGTableLayer::ResolveSRID(const OGRPGGeomFieldDefn *poGFldDefn)

{
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;

    int nSRSId = poDS->GetUndefinedSRID();
    if (!poDS->m_bHasGeometryColumns)
    {
        poGFldDefn->nSRSId = nSRSId;
        return;
    }

    osCommand.Printf(
        "SELECT srid FROM geometry_columns "
        "WHERE f_table_name = %s AND "
        "f_geometry_column = %s",
        OGRPGEscapeString(hPGConn, pszTableName).c_str(),
        OGRPGEscapeString(hPGConn, poGFldDefn->GetNameRef()).c_str());

    osCommand +=
        CPLString().Printf(" AND f_table_schema = %s",
                           OGRPGEscapeString(hPGConn, pszSchemaName).c_str());

    PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());

    if (hResult && PQresultStatus(hResult) == PGRES_TUPLES_OK &&
        PQntuples(hResult) == 1)
    {
        nSRSId = atoi(PQgetvalue(hResult, 0, 0));
    }

    OGRPGClearResult(hResult);

    /* With PostGIS 2.0, SRID = 0 can also mean that there's no constraint */
    /* so we need to fetch from values */
    /* We assume that all geometry of this column have identical SRID */
    if (nSRSId <= 0 && poGFldDefn->ePostgisType == GEOM_TYPE_GEOMETRY &&
        poDS->sPostGISVersion.nMajor >= 0)
    {
        CPLString osGetSRID;
        osGetSRID += "SELECT ST_SRID(";
        osGetSRID += OGRPGEscapeColumnName(poGFldDefn->GetNameRef());
        osGetSRID += ") FROM ";
        osGetSRID += pszSqlTableName;
        osGetSRID += " WHERE (";
        osGetSRID += OGRPGEscapeColumnName(poGFldDefn->GetNameRef());
        osGetSRID += " IS NOT NULL) LIMIT 1";

        hResult = OGRPG_PQexec(poDS->GetPGConn(), osGetSRID);
        if (hResult && PQresultStatus(hResult) == PGRES_TUPLES_OK &&
            PQntuples(hResult) == 1)
        {
            nSRSId = atoi(PQgetvalue(hResult, 0, 0));
        }

        OGRPGClearResult(hResult);
    }

    poGFldDefn->nSRSId = nSRSId;
}

/************************************************************************/
/*                             StartCopy()                              */
/************************************************************************/

OGRErr OGRPGTableLayer::StartCopy()

{
    /*CPLDebug("PG", "OGRPGDataSource(%p)::StartCopy(%p)", poDS, this);*/

    CPLString osFields = BuildCopyFields();

    size_t size = osFields.size() + strlen(pszSqlTableName) + 100;
    char *pszCommand = static_cast<char *>(CPLMalloc(size));

    snprintf(pszCommand, size, "COPY %s (%s) FROM STDIN;", pszSqlTableName,
             osFields.c_str());

    PGconn *hPGConn = poDS->GetPGConn();
    PGresult *hResult = OGRPG_PQexec(hPGConn, pszCommand);

    if (!hResult || (PQresultStatus(hResult) != PGRES_COPY_IN))
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s", PQerrorMessage(hPGConn));
    }
    else
        bCopyActive = TRUE;

    OGRPGClearResult(hResult);
    CPLFree(pszCommand);

    return OGRERR_NONE;
}

/************************************************************************/
/*                              EndCopy()                               */
/************************************************************************/

OGRErr OGRPGTableLayer::EndCopy()

{
    if (!bCopyActive)
        return OGRERR_NONE;
    /*CPLDebug("PG", "OGRPGDataSource(%p)::EndCopy(%p)", poDS, this);*/

    /* This method is called from the datasource when
       a COPY operation is ended */
    OGRErr result = OGRERR_NONE;

    PGconn *hPGConn = poDS->GetPGConn();
    CPLDebug("PG", "PQputCopyEnd()");

    bCopyActive = FALSE;

    int copyResult = PQputCopyEnd(hPGConn, nullptr);

    switch (copyResult)
    {
        case 0:
            CPLError(CE_Failure, CPLE_AppDefined, "Writing COPY data blocked.");
            result = OGRERR_FAILURE;
            break;
        case -1:
            CPLError(CE_Failure, CPLE_AppDefined, "%s",
                     PQerrorMessage(hPGConn));
            result = OGRERR_FAILURE;
            break;
    }

    /* Now check the results of the copy */
    PGresult *hResult = PQgetResult(hPGConn);

    if (hResult && PQresultStatus(hResult) != PGRES_COMMAND_OK)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "COPY statement failed.\n%s",
                 PQerrorMessage(hPGConn));

        result = OGRERR_FAILURE;
    }

    OGRPGClearResult(hResult);

    if (!bUseCopyByDefault)
        bUseCopy = USE_COPY_UNSET;

    UpdateSequenceIfNeeded();

    return result;
}

/************************************************************************/
/*                       UpdateSequenceIfNeeded()                       */
/************************************************************************/

void OGRPGTableLayer::UpdateSequenceIfNeeded()
{
    if (bNeedToUpdateSequence && pszFIDColumn != nullptr)
    {
        PGconn *hPGConn = poDS->GetPGConn();
        CPLString osCommand;
        // setval() only works if the value is in [1,INT_MAX] range
        // so do not update it if MAX(fid) <= 0
        osCommand.Printf(
            "SELECT setval(pg_get_serial_sequence(%s, %s), MAX(%s)) FROM %s "
            "WHERE EXISTS (SELECT 1 FROM %s WHERE %s > 0 LIMIT 1)",
            OGRPGEscapeString(hPGConn, pszSqlTableName).c_str(),
            OGRPGEscapeString(hPGConn, pszFIDColumn).c_str(),
            OGRPGEscapeColumnName(pszFIDColumn).c_str(), pszSqlTableName,
            pszSqlTableName, OGRPGEscapeColumnName(pszFIDColumn).c_str());
        PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);
        OGRPGClearResult(hResult);
        bNeedToUpdateSequence = false;
    }
}

/************************************************************************/
/*                          BuildCopyFields()                           */
/************************************************************************/

CPLString OGRPGTableLayer::BuildCopyFields()
{
    int i = 0;
    int nFIDIndex = -1;
    CPLString osFieldList;

    for (i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
    {
        OGRGeomFieldDefn *poGeomFieldDefn = poFeatureDefn->GetGeomFieldDefn(i);
        if (!osFieldList.empty())
            osFieldList += ", ";
        osFieldList += OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef());
    }

    if (bFIDColumnInCopyFields)
    {
        if (!osFieldList.empty())
            osFieldList += ", ";

        nFIDIndex = poFeatureDefn->GetFieldIndex(pszFIDColumn);

        osFieldList += OGRPGEscapeColumnName(pszFIDColumn);
    }

    for (i = 0; i < poFeatureDefn->GetFieldCount(); i++)
    {
        if (i == nFIDIndex)
            continue;

        if (poFeatureDefn->GetFieldDefn(i)->IsGenerated())
            continue;

        const char *pszName = poFeatureDefn->GetFieldDefn(i)->GetNameRef();

        if (!osFieldList.empty())
            osFieldList += ", ";

        osFieldList += OGRPGEscapeColumnName(pszName);
    }

    return osFieldList;
}

/************************************************************************/
/*                    CheckGeomTypeCompatibility()                      */
/************************************************************************/

void OGRPGTableLayer::CheckGeomTypeCompatibility(int iGeomField,
                                                 OGRGeometry *poGeom)
{
    if (bHasWarnedIncompatibleGeom)
        return;

    OGRwkbGeometryType eExpectedGeomType =
        poFeatureDefn->GetGeomFieldDefn(iGeomField)->GetType();
    OGRwkbGeometryType eFlatLayerGeomType = wkbFlatten(eExpectedGeomType);
    OGRwkbGeometryType eFlatGeomType = wkbFlatten(poGeom->getGeometryType());
    if (eFlatLayerGeomType == wkbUnknown)
        return;

    if (eFlatLayerGeomType == wkbGeometryCollection)
        bHasWarnedIncompatibleGeom = eFlatGeomType != wkbMultiPoint &&
                                     eFlatGeomType != wkbMultiLineString &&
                                     eFlatGeomType != wkbMultiPolygon &&
                                     eFlatGeomType != wkbGeometryCollection;
    else
        bHasWarnedIncompatibleGeom = (eFlatGeomType != eFlatLayerGeomType);

    if (bHasWarnedIncompatibleGeom)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Geometry to be inserted is of type %s, whereas the layer "
                 "geometry type is %s.\n"
                 "Insertion is likely to fail",
                 OGRGeometryTypeToName(poGeom->getGeometryType()),
                 OGRGeometryTypeToName(eExpectedGeomType));
    }
}

/************************************************************************/
/*                        SetOverrideColumnTypes()                      */
/************************************************************************/

void OGRPGTableLayer::SetOverrideColumnTypes(const char *pszOverrideColumnTypes)
{
    if (pszOverrideColumnTypes == nullptr)
        return;

    const char *pszIter = pszOverrideColumnTypes;
    CPLString osCur;
    while (*pszIter != '\0')
    {
        if (*pszIter == '(')
        {
            /* Ignore commas inside ( ) pair */
            while (*pszIter != '\0')
            {
                if (*pszIter == ')')
                {
                    osCur += *pszIter;
                    pszIter++;
                    break;
                }
                osCur += *pszIter;
                pszIter++;
            }
            if (*pszIter == '\0')
                break;
        }

        if (*pszIter == ',')
        {
            papszOverrideColumnTypes =
                CSLAddString(papszOverrideColumnTypes, osCur);
            osCur = "";
        }
        else
            osCur += *pszIter;
        pszIter++;
    }
    if (!osCur.empty())
        papszOverrideColumnTypes =
            CSLAddString(papszOverrideColumnTypes, osCur);
}

/************************************************************************/
/*                            IGetExtent()                              */
/*                                                                      */
/*      For PostGIS use internal ST_EstimatedExtent(geometry) function  */
/*      if bForce == 0                                                  */
/************************************************************************/

OGRErr OGRPGTableLayer::IGetExtent(int iGeomField, OGREnvelope *psExtent,
                                   bool bForce)
{
    CPLString osCommand;

    if (bDeferredCreation && RunDeferredCreationIfNecessary() != OGRERR_NONE)
        return OGRERR_FAILURE;
    poDS->EndCopy();

    OGRPGGeomFieldDefn *poGeomFieldDefn =
        poFeatureDefn->GetGeomFieldDefn(iGeomField);

    // if bForce is 0 and ePostgisType is not GEOM_TYPE_GEOGRAPHY we can use
    // the ST_EstimatedExtent function which is quicker
    // ST_EstimatedExtent was called ST_Estimated_Extent up to PostGIS 2.0.x
    // ST_EstimatedExtent returns NULL in absence of statistics (an exception
    // before
    //   PostGIS 1.5.4)
    if (bForce == 0 && TestCapability(OLCFastGetExtent))
    {
        PGconn *hPGConn = poDS->GetPGConn();

        const char *pszExtentFct = poDS->sPostGISVersion.nMajor > 2 ||
                                           (poDS->sPostGISVersion.nMajor == 2 &&
                                            poDS->sPostGISVersion.nMinor >= 1)
                                       ? "ST_EstimatedExtent"
                                       : "ST_Estimated_Extent";

        osCommand.Printf(
            "SELECT %s(%s, %s, %s)", pszExtentFct,
            OGRPGEscapeString(hPGConn, pszSchemaName).c_str(),
            OGRPGEscapeString(hPGConn, pszTableName).c_str(),
            OGRPGEscapeString(hPGConn, poGeomFieldDefn->GetNameRef()).c_str());

        /* Quiet error: ST_Estimated_Extent may return an error if statistics */
        /* have not been computed */
        if (RunGetExtentRequest(*psExtent, bForce, osCommand, TRUE) ==
            OGRERR_NONE)
            return OGRERR_NONE;

        CPLDebug(
            "PG",
            "Unable to get estimated extent by PostGIS. Trying real extent.");
    }

    return OGRPGLayer::IGetExtent(iGeomField, psExtent, bForce);
}

/************************************************************************/
/*                           Rename()                                   */
/************************************************************************/

OGRErr OGRPGTableLayer::Rename(const char *pszNewName)
{
    if (!TestCapability(OLCRename))
        return OGRERR_FAILURE;

    if (bDeferredCreation && RunDeferredCreationIfNecessary() != OGRERR_NONE)
        return OGRERR_FAILURE;
    poDS->EndCopy();
    ResetReading();

    char *pszNewSqlTableName = CPLStrdup(OGRPGEscapeColumnName(pszNewName));
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;
    osCommand.Printf("ALTER TABLE %s RENAME TO %s", pszSqlTableName,
                     pszNewSqlTableName);
    PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand);

    OGRErr eRet = OGRERR_NONE;
    if (!hResult || PQresultStatus(hResult) != PGRES_COMMAND_OK)
    {
        eRet = OGRERR_FAILURE;
        CPLError(CE_Failure, CPLE_AppDefined, "%s", PQerrorMessage(hPGConn));

        CPLFree(pszNewSqlTableName);
    }
    else
    {
        CPLFree(pszTableName);
        pszTableName = CPLStrdup(pszNewName);

        CPLFree(pszSqlTableName);
        pszSqlTableName = pszNewSqlTableName;

        SetDescription(pszNewName);
        whileUnsealing(poFeatureDefn)->SetName(pszNewName);
    }

    OGRPGClearResult(hResult);

    return eRet;
}

/************************************************************************/
/*                        SetDeferredCreation()                         */
/************************************************************************/

void OGRPGTableLayer::SetDeferredCreation(int bDeferredCreationIn,
                                          const std::string &osCreateTableIn)
{
    bDeferredCreation = bDeferredCreationIn;
    osCreateTable = osCreateTableIn;
}

/************************************************************************/
/*                      RunDeferredCreationIfNecessary()                */
/************************************************************************/

OGRErr OGRPGTableLayer::RunDeferredCreationIfNecessary()
{
    if (!bDeferredCreation)
        return OGRERR_NONE;
    bDeferredCreation = FALSE;

    poDS->EndCopy();

    for (int i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
    {
        OGRPGGeomFieldDefn *poGeomField = poFeatureDefn->GetGeomFieldDefn(i);

        if (poDS->HavePostGIS() ||
            poGeomField->ePostgisType == GEOM_TYPE_GEOGRAPHY)
        {
            const char *pszGeometryType =
                OGRToOGCGeomType(poGeomField->GetType());

            osCreateTable += ", ";
            osCreateTable += OGRPGEscapeColumnName(poGeomField->GetNameRef());
            osCreateTable += " ";
            if (poGeomField->ePostgisType == GEOM_TYPE_GEOMETRY)
                osCreateTable += "geometry(";
            else
                osCreateTable += "geography(";
            osCreateTable += pszGeometryType;
            if ((poGeomField->GeometryTypeFlags & OGRGeometry::OGR_G_3D) &&
                (poGeomField->GeometryTypeFlags & OGRGeometry::OGR_G_MEASURED))
                osCreateTable += "ZM";
            else if (poGeomField->GeometryTypeFlags & OGRGeometry::OGR_G_3D)
                osCreateTable += "Z";
            else if (poGeomField->GeometryTypeFlags &
                     OGRGeometry::OGR_G_MEASURED)
                osCreateTable += "M";
            if (poGeomField->nSRSId > 0)
                osCreateTable += CPLSPrintf(",%d", poGeomField->nSRSId);
            osCreateTable += ")";
            if (!poGeomField->IsNullable())
                osCreateTable += " NOT NULL";
        }
    }

    osCreateTable += " )";
    CPLString osCommand(osCreateTable);

    PGconn *hPGConn = poDS->GetPGConn();

    PGresult *hResult = OGRPG_PQexec(hPGConn, osCommand.c_str());
    if (PQresultStatus(hResult) != PGRES_COMMAND_OK)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s\n%s", osCommand.c_str(),
                 PQerrorMessage(hPGConn));

        OGRPGClearResult(hResult);
        return OGRERR_FAILURE;
    }

    OGRPGClearResult(hResult);

    for (const auto &osSQL : m_aosDeferredCommentOnColumns)
    {
        hResult = OGRPG_PQexec(hPGConn, osSQL.c_str());
        OGRPGClearResult(hResult);
    }
    m_aosDeferredCommentOnColumns.clear();

    if (bCreateSpatialIndexFlag)
    {
        for (int i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
        {
            OGRPGGeomFieldDefn *poGeomField =
                poFeatureDefn->GetGeomFieldDefn(i);
            if (RunCreateSpatialIndex(poGeomField, i) != OGRERR_NONE)
            {
                return OGRERR_FAILURE;
            }
        }
    }

    char **papszMD = OGRLayer::GetMetadata();
    if (papszMD != nullptr)
        SetMetadata(papszMD);

    return OGRERR_NONE;
}

/************************************************************************/
/*                         GetGeometryTypes()                           */
/************************************************************************/

OGRGeometryTypeCounter *OGRPGTableLayer::GetGeometryTypes(
    int iGeomField, int nFlagsGGT, int &nEntryCountOut,
    GDALProgressFunc pfnProgress, void *pProgressData)
{
    if (iGeomField < 0 || iGeomField >= GetLayerDefn()->GetGeomFieldCount())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Invalid geometry field index : %d", iGeomField);
        nEntryCountOut = 0;
        return nullptr;
    }

    if (bDeferredCreation && RunDeferredCreationIfNecessary() != OGRERR_NONE)
    {
        nEntryCountOut = 0;
        return nullptr;
    }
    poDS->EndCopy();

    const OGRPGGeomFieldDefn *poGeomFieldDefn =
        GetLayerDefn()->GetGeomFieldDefn(iGeomField);
    const auto osEscapedGeom =
        OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef());
    CPLString osSQL;
    if ((nFlagsGGT & OGR_GGT_GEOMCOLLECTIONZ_TINZ) != 0)
    {
        CPLString osFilter;
        osFilter.Printf("(ST_Zmflag(%s) = 2 AND "
                        "((GeometryType(%s) = 'GEOMETRYCOLLECTION' AND "
                        "ST_NumGeometries(%s) >= 1 AND "
                        "geometrytype(ST_GeometryN(%s, 1)) = 'TIN') OR "
                        "GeometryType(%s) = 'TIN'))",
                        osEscapedGeom.c_str(), osEscapedGeom.c_str(),
                        osEscapedGeom.c_str(), osEscapedGeom.c_str(),
                        osEscapedGeom.c_str());

        std::string l_osWHERE(osWHERE);
        if (l_osWHERE.empty())
            l_osWHERE = " WHERE ";
        else
            l_osWHERE += " AND ";
        l_osWHERE += "(NOT (";
        l_osWHERE += osFilter;
        l_osWHERE += ") OR ";
        l_osWHERE += osEscapedGeom;
        l_osWHERE += " IS NULL)";

        std::string l_osWHEREFilter(osWHERE);
        if (l_osWHEREFilter.empty())
            l_osWHEREFilter = " WHERE ";
        else
            l_osWHEREFilter += " AND ";
        l_osWHEREFilter += osFilter;

        osSQL.Printf(
            "(SELECT GeometryType(%s), ST_Zmflag(%s), COUNT(*) FROM %s %s "
            "GROUP BY GeometryType(%s), ST_Zmflag(%s)) UNION ALL "
            "(SELECT * FROM (SELECT 'TIN', 2, COUNT(*) AS count FROM %s %s) "
            "tinsubselect WHERE tinsubselect.count != 0)",
            osEscapedGeom.c_str(), osEscapedGeom.c_str(), pszSqlTableName,
            l_osWHERE.c_str(), osEscapedGeom.c_str(), osEscapedGeom.c_str(),
            pszSqlTableName, l_osWHEREFilter.c_str());
    }
    else if ((nFlagsGGT & OGR_GGT_STOP_IF_MIXED) != 0)
    {
        std::string l_osWHERE(osWHERE);
        if (l_osWHERE.empty())
            l_osWHERE = " WHERE ";
        else
            l_osWHERE += " AND ";
        l_osWHERE += osEscapedGeom;
        l_osWHERE += " IS NOT NULL";

        std::string l_osWHERE_NULL(osWHERE);
        if (l_osWHERE_NULL.empty())
            l_osWHERE_NULL = " WHERE ";
        else
            l_osWHERE_NULL += " AND ";
        l_osWHERE_NULL += osEscapedGeom;
        l_osWHERE_NULL += " IS NULL";

        osSQL.Printf("(SELECT DISTINCT GeometryType(%s), ST_Zmflag(%s), 0 FROM "
                     "%s %s LIMIT 2) "
                     "UNION ALL (SELECT NULL, NULL, 0 FROM %s %s LIMIT 1)",
                     osEscapedGeom.c_str(), osEscapedGeom.c_str(),
                     pszSqlTableName, l_osWHERE.c_str(), pszSqlTableName,
                     l_osWHERE_NULL.c_str());
    }
    else
    {
        const bool bDebug =
            CPLTestBool(CPLGetConfigOption("OGR_PG_DEBUG_GGT_CANCEL", "NO"));
        osSQL.Printf(
            "SELECT GeometryType(%s), ST_Zmflag(%s), COUNT(*)%s FROM %s %s "
            "GROUP BY GeometryType(%s), ST_Zmflag(%s)",
            osEscapedGeom.c_str(), osEscapedGeom.c_str(),
            bDebug ? ", pg_sleep(1)" : "", pszSqlTableName, osWHERE.c_str(),
            osEscapedGeom.c_str(), osEscapedGeom.c_str());
    }

    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool stopThread = false;
    if (pfnProgress && pfnProgress != GDALDummyProgress)
    {
        thread = std::thread(
            [&]()
            {
                std::unique_lock<std::mutex> lock(mutex);
                while (!stopThread)
                {
                    if (!pfnProgress(0.0, "", pProgressData))
                        poDS->AbortSQL();
                    cv.wait_for(lock, std::chrono::milliseconds(100));
                }
            });
    }

    PGconn *hPGConn = poDS->GetPGConn();
    PGresult *hResult = OGRPG_PQexec(hPGConn, osSQL.c_str());

    if (pfnProgress && pfnProgress != GDALDummyProgress)
    {
        {
            std::unique_lock<std::mutex> lock(mutex);
            stopThread = true;
            cv.notify_one();
        }
        thread.join();
    }

    nEntryCountOut = 0;
    OGRGeometryTypeCounter *pasRet = nullptr;
    if (hResult && PQresultStatus(hResult) == PGRES_TUPLES_OK)
    {
        const int nTuples = PQntuples(hResult);
        nEntryCountOut = nTuples;
        pasRet = static_cast<OGRGeometryTypeCounter *>(
            CPLCalloc(1 + nEntryCountOut, sizeof(OGRGeometryTypeCounter)));
        for (int i = 0; i < nTuples; ++i)
        {
            const char *pszGeomType = PQgetvalue(hResult, i, 0);
            const char *pszZMFlag = PQgetvalue(hResult, i, 1);
            const char *pszCount = PQgetvalue(hResult, i, 2);
            if (pszCount)
            {
                if (pszGeomType == nullptr || pszGeomType[0] == '\0')
                {
                    pasRet[i].eGeomType = wkbNone;
                }
                else if (pszZMFlag != nullptr)
                {
                    const int nZMFlag = atoi(pszZMFlag);
                    pasRet[i].eGeomType = OGRFromOGCGeomType(pszGeomType);
                    int nModifier = 0;
                    if (nZMFlag == 1)
                        nModifier = OGRGeometry::OGR_G_MEASURED;
                    else if (nZMFlag == 2)
                        nModifier = OGRGeometry::OGR_G_3D;
                    else if (nZMFlag == 3)
                        nModifier =
                            OGRGeometry::OGR_G_MEASURED | OGRGeometry::OGR_G_3D;
                    pasRet[i].eGeomType = OGR_GT_SetModifier(
                        pasRet[i].eGeomType, nModifier & OGRGeometry::OGR_G_3D,
                        nModifier & OGRGeometry::OGR_G_MEASURED);
                }
                pasRet[i].nCount =
                    static_cast<int64_t>(std::strtoll(pszCount, nullptr, 10));
            }
        }
    }

    OGRPGClearResult(hResult);

    return pasRet;
}

/************************************************************************/
/*                          FindFieldIndex()                            */
/************************************************************************/

int OGRPGTableLayer::FindFieldIndex(const char *pszFieldName, int bExactMatch)
{
    const auto poLayerDefn = GetLayerDefn();
    int iField = poLayerDefn->GetFieldIndex(pszFieldName);

    if (!bExactMatch && iField < 0 && bLaunderColumnNames)
    {
        CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
        char *pszSafeName =
            OGRPGCommonLaunderName(pszFieldName, "PG", m_bUTF8ToASCII);
        iField = poLayerDefn->GetFieldIndex(pszSafeName);
        CPLFree(pszSafeName);
    }

    return iField;
}

#undef PQexec
