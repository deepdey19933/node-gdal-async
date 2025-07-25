/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements a few base methods on OGRGeometry.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999, Frank Warmerdam
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "ogr_geometry.h"

#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_multiproc.h"
#include "cpl_string.h"
#include "ogr_api.h"
#include "ogr_core.h"
#include "ogr_geos.h"
#include "ogr_sfcgal.h"
#include "ogr_libs.h"
#include "ogr_p.h"
#include "ogr_spatialref.h"
#include "ogr_srs_api.h"
#include "ogr_wkb.h"

#ifndef HAVE_GEOS
#define UNUSED_IF_NO_GEOS CPL_UNUSED
#else
#define UNUSED_IF_NO_GEOS
#endif

//! @cond Doxygen_Suppress
int OGRGeometry::bGenerate_DB2_V72_BYTE_ORDER = FALSE;
//! @endcond

#ifdef HAVE_GEOS
static void OGRGEOSErrorHandler(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    CPLErrorV(CE_Failure, CPLE_AppDefined, fmt, args);
    va_end(args);
}

static void OGRGEOSWarningHandler(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    CPLErrorV(CE_Warning, CPLE_AppDefined, fmt, args);
    va_end(args);
}
#endif

/************************************************************************/
/*                            OGRWktOptions()                             */
/************************************************************************/

int OGRWktOptions::getDefaultPrecision()
{
    return atoi(CPLGetConfigOption("OGR_WKT_PRECISION", "15"));
}

bool OGRWktOptions::getDefaultRound()
{
    return CPLTestBool(CPLGetConfigOption("OGR_WKT_ROUND", "TRUE"));
}

/************************************************************************/
/*                            OGRGeometry()                             */
/************************************************************************/

OGRGeometry::OGRGeometry() = default;

/************************************************************************/
/*                   OGRGeometry( const OGRGeometry& )                  */
/************************************************************************/

/**
 * \brief Copy constructor.
 *
 * Note: before GDAL 2.1, only the default implementation of the constructor
 * existed, which could be unsafe to use.
 *
 * @since GDAL 2.1
 */

OGRGeometry::OGRGeometry(const OGRGeometry &other)
    : poSRS(other.poSRS), flags(other.flags)
{
    if (poSRS != nullptr)
        const_cast<OGRSpatialReference *>(poSRS)->Reference();
}

/************************************************************************/
/*                   OGRGeometry( OGRGeometry&& )                       */
/************************************************************************/

/**
 * \brief Move constructor.
 *
 * @since GDAL 3.11
 */

OGRGeometry::OGRGeometry(OGRGeometry &&other)
    : poSRS(other.poSRS), flags(other.flags)
{
    other.poSRS = nullptr;
}

/************************************************************************/
/*                            ~OGRGeometry()                            */
/************************************************************************/

OGRGeometry::~OGRGeometry()

{
    if (poSRS != nullptr)
        const_cast<OGRSpatialReference *>(poSRS)->Release();
}

/************************************************************************/
/*                    operator=( const OGRGeometry&)                    */
/************************************************************************/

/**
 * \brief Assignment operator.
 *
 * Note: before GDAL 2.1, only the default implementation of the operator
 * existed, which could be unsafe to use.
 *
 * @since GDAL 2.1
 */

OGRGeometry &OGRGeometry::operator=(const OGRGeometry &other)
{
    if (this != &other)
    {
        empty();
        assignSpatialReference(other.getSpatialReference());
        flags = other.flags;
    }
    return *this;
}

/************************************************************************/
/*                    operator=( OGRGeometry&&)                         */
/************************************************************************/

/**
 * \brief Move assignment operator.
 *
 * @since GDAL 3.11
 */

OGRGeometry &OGRGeometry::operator=(OGRGeometry &&other)
{
    if (this != &other)
    {
        poSRS = other.poSRS;
        other.poSRS = nullptr;
        flags = other.flags;
    }
    return *this;
}

/************************************************************************/
/*                            dumpReadable()                            */
/************************************************************************/

/**
 * \brief Dump geometry in well known text format to indicated output file.
 *
 * A few options can be defined to change the default dump :
 * <ul>
 * <li>DISPLAY_GEOMETRY=NO : to hide the dump of the geometry</li>
 * <li>DISPLAY_GEOMETRY=WKT or YES (default) : dump the geometry as a WKT</li>
 * <li>DISPLAY_GEOMETRY=SUMMARY : to get only a summary of the geometry</li>
 * </ul>
 *
 * This method is the same as the C function OGR_G_DumpReadable().
 *
 * @param fp the text file to write the geometry to.
 * @param pszPrefix the prefix to put on each line of output.
 * @param papszOptions NULL terminated list of options (may be NULL)
 */

void OGRGeometry::dumpReadable(FILE *fp, const char *pszPrefix,
                               CSLConstList papszOptions) const

{
    if (fp == nullptr)
        fp = stdout;

    const auto osStr = dumpReadable(pszPrefix, papszOptions);
    fprintf(fp, "%s", osStr.c_str());
}

/************************************************************************/
/*                            dumpReadable()                            */
/************************************************************************/

/**
 * \brief Dump geometry in well known text format to indicated output file.
 *
 * A few options can be defined to change the default dump :
 * <ul>
 * <li>DISPLAY_GEOMETRY=NO : to hide the dump of the geometry</li>
 * <li>DISPLAY_GEOMETRY=WKT or YES (default) : dump the geometry as a WKT</li>
 * <li>DISPLAY_GEOMETRY=SUMMARY : to get only a summary of the geometry</li>
 * <li>XY_COORD_PRECISION=integer: number of decimal figures for X,Y coordinates
 * in WKT (added in GDAL 3.9)</li>
 * <li>Z_COORD_PRECISION=integer: number of decimal figures for Z coordinates in
 * WKT (added in GDAL 3.9)</li>
 * </ul>
 *
 * @param pszPrefix the prefix to put on each line of output.
 * @param papszOptions NULL terminated list of options (may be NULL)
 * @return a string with the geometry representation.
 * @since GDAL 3.7
 */

std::string OGRGeometry::dumpReadable(const char *pszPrefix,
                                      CSLConstList papszOptions) const

{
    if (pszPrefix == nullptr)
        pszPrefix = "";

    std::string osRet;

    const auto exportToWktWithOpts =
        [this, pszPrefix, papszOptions, &osRet](bool bIso)
    {
        OGRErr err(OGRERR_NONE);
        OGRWktOptions opts;
        if (const char *pszXYPrecision =
                CSLFetchNameValue(papszOptions, "XY_COORD_PRECISION"))
        {
            opts.format = OGRWktFormat::F;
            opts.xyPrecision = atoi(pszXYPrecision);
        }
        if (const char *pszZPrecision =
                CSLFetchNameValue(papszOptions, "Z_COORD_PRECISION"))
        {
            opts.format = OGRWktFormat::F;
            opts.zPrecision = atoi(pszZPrecision);
        }
        if (bIso)
            opts.variant = wkbVariantIso;
        std::string wkt = exportToWkt(opts, &err);
        if (err == OGRERR_NONE)
        {
            osRet = pszPrefix;
            osRet += wkt.data();
            osRet += '\n';
        }
    };

    const char *pszDisplayGeometry =
        CSLFetchNameValue(papszOptions, "DISPLAY_GEOMETRY");
    if (pszDisplayGeometry != nullptr && EQUAL(pszDisplayGeometry, "SUMMARY"))
    {
        osRet += CPLOPrintf("%s%s : ", pszPrefix, getGeometryName());
        switch (getGeometryType())
        {
            case wkbUnknown:
            case wkbNone:
            case wkbPoint:
            case wkbPoint25D:
            case wkbPointM:
            case wkbPointZM:
                break;
            case wkbPolyhedralSurface:
            case wkbTIN:
            case wkbPolyhedralSurfaceZ:
            case wkbTINZ:
            case wkbPolyhedralSurfaceM:
            case wkbTINM:
            case wkbPolyhedralSurfaceZM:
            case wkbTINZM:
            {
                const OGRPolyhedralSurface *poPS = toPolyhedralSurface();
                osRet +=
                    CPLOPrintf("%d geometries:\n", poPS->getNumGeometries());
                for (auto &&poSubGeom : *poPS)
                {
                    osRet += pszPrefix;
                    osRet += poSubGeom->dumpReadable(pszPrefix, papszOptions);
                }
                break;
            }
            case wkbLineString:
            case wkbLineString25D:
            case wkbLineStringM:
            case wkbLineStringZM:
            case wkbCircularString:
            case wkbCircularStringZ:
            case wkbCircularStringM:
            case wkbCircularStringZM:
            {
                const OGRSimpleCurve *poSC = toSimpleCurve();
                osRet += CPLOPrintf("%d points\n", poSC->getNumPoints());
                break;
            }
            case wkbPolygon:
            case wkbTriangle:
            case wkbTriangleZ:
            case wkbTriangleM:
            case wkbTriangleZM:
            case wkbPolygon25D:
            case wkbPolygonM:
            case wkbPolygonZM:
            case wkbCurvePolygon:
            case wkbCurvePolygonZ:
            case wkbCurvePolygonM:
            case wkbCurvePolygonZM:
            {
                const OGRCurvePolygon *poPoly = toCurvePolygon();
                const OGRCurve *poRing = poPoly->getExteriorRingCurve();
                const int nRings = poPoly->getNumInteriorRings();
                if (poRing == nullptr)
                {
                    osRet += "empty";
                }
                else
                {
                    osRet += CPLOPrintf("%d points", poRing->getNumPoints());
                    if (wkbFlatten(poRing->getGeometryType()) ==
                        wkbCompoundCurve)
                    {
                        osRet += " (";
                        osRet += poRing->dumpReadable(nullptr, papszOptions);
                        osRet += ")";
                    }
                    if (nRings)
                    {
                        osRet += CPLOPrintf(", %d inner rings (", nRings);
                        for (int ir = 0; ir < nRings; ir++)
                        {
                            poRing = poPoly->getInteriorRingCurve(ir);
                            if (ir)
                                osRet += ", ";
                            osRet +=
                                CPLOPrintf("%d points", poRing->getNumPoints());
                            if (wkbFlatten(poRing->getGeometryType()) ==
                                wkbCompoundCurve)
                            {
                                osRet += " (";
                                osRet +=
                                    poRing->dumpReadable(nullptr, papszOptions);
                                osRet += ")";
                            }
                        }
                        osRet += ")";
                    }
                }
                osRet += "\n";
                break;
            }
            case wkbCompoundCurve:
            case wkbCompoundCurveZ:
            case wkbCompoundCurveM:
            case wkbCompoundCurveZM:
            {
                const OGRCompoundCurve *poCC = toCompoundCurve();
                if (poCC->getNumCurves() == 0)
                {
                    osRet += "empty";
                }
                else
                {
                    for (int i = 0; i < poCC->getNumCurves(); i++)
                    {
                        if (i)
                            osRet += ", ";
                        osRet +=
                            CPLOPrintf("%s (%d points)",
                                       poCC->getCurve(i)->getGeometryName(),
                                       poCC->getCurve(i)->getNumPoints());
                    }
                }
                break;
            }

            case wkbMultiPoint:
            case wkbMultiLineString:
            case wkbMultiPolygon:
            case wkbMultiCurve:
            case wkbMultiSurface:
            case wkbGeometryCollection:
            case wkbMultiPoint25D:
            case wkbMultiLineString25D:
            case wkbMultiPolygon25D:
            case wkbMultiCurveZ:
            case wkbMultiSurfaceZ:
            case wkbGeometryCollection25D:
            case wkbMultiPointM:
            case wkbMultiLineStringM:
            case wkbMultiPolygonM:
            case wkbMultiCurveM:
            case wkbMultiSurfaceM:
            case wkbGeometryCollectionM:
            case wkbMultiPointZM:
            case wkbMultiLineStringZM:
            case wkbMultiPolygonZM:
            case wkbMultiCurveZM:
            case wkbMultiSurfaceZM:
            case wkbGeometryCollectionZM:
            {
                const OGRGeometryCollection *poColl = toGeometryCollection();
                osRet +=
                    CPLOPrintf("%d geometries:\n", poColl->getNumGeometries());
                for (auto &&poSubGeom : *poColl)
                {
                    osRet += pszPrefix;
                    osRet += poSubGeom->dumpReadable(pszPrefix, papszOptions);
                }
                break;
            }
            case wkbLinearRing:
            case wkbCurve:
            case wkbSurface:
            case wkbCurveZ:
            case wkbSurfaceZ:
            case wkbCurveM:
            case wkbSurfaceM:
            case wkbCurveZM:
            case wkbSurfaceZM:
                break;
        }
    }
    else if (pszDisplayGeometry != nullptr && EQUAL(pszDisplayGeometry, "WKT"))
    {
        exportToWktWithOpts(/* bIso=*/false);
    }
    else if (pszDisplayGeometry == nullptr || CPLTestBool(pszDisplayGeometry) ||
             EQUAL(pszDisplayGeometry, "ISO_WKT"))
    {
        exportToWktWithOpts(/* bIso=*/true);
    }

    return osRet;
}

/************************************************************************/
/*                         OGR_G_DumpReadable()                         */
/************************************************************************/
/**
 * \brief Dump geometry in well known text format to indicated output file.
 *
 * This method is the same as the CPP method OGRGeometry::dumpReadable.
 *
 * @param hGeom handle on the geometry to dump.
 * @param fp the text file to write the geometry to.
 * @param pszPrefix the prefix to put on each line of output.
 */

void OGR_G_DumpReadable(OGRGeometryH hGeom, FILE *fp, const char *pszPrefix)

{
    VALIDATE_POINTER0(hGeom, "OGR_G_DumpReadable");

    OGRGeometry::FromHandle(hGeom)->dumpReadable(fp, pszPrefix);
}

/************************************************************************/
/*                       assignSpatialReference()                       */
/************************************************************************/

/**
 * \brief Assign spatial reference to this object.
 *
 * Any existing spatial reference
 * is replaced, but under no circumstances does this result in the object
 * being reprojected.  It is just changing the interpretation of the existing
 * geometry.  Note that assigning a spatial reference increments the
 * reference count on the OGRSpatialReference, but does not copy it.
 *
 * Starting with GDAL 2.3, this will also assign the spatial reference to
 * potential sub-geometries of the geometry (OGRGeometryCollection,
 * OGRCurvePolygon/OGRPolygon, OGRCompoundCurve, OGRPolyhedralSurface and their
 * derived classes).
 *
 * This is similar to the SFCOM IGeometry::put_SpatialReference() method.
 *
 * This method is the same as the C function OGR_G_AssignSpatialReference().
 *
 * @param poSR new spatial reference system to apply.
 */

void OGRGeometry::assignSpatialReference(const OGRSpatialReference *poSR)

{
    // Do in that order to properly handle poSR == poSRS
    if (poSR != nullptr)
        const_cast<OGRSpatialReference *>(poSR)->Reference();
    if (poSRS != nullptr)
        const_cast<OGRSpatialReference *>(poSRS)->Release();

    poSRS = poSR;
}

/************************************************************************/
/*                    OGR_G_AssignSpatialReference()                    */
/************************************************************************/
/**
 * \brief Assign spatial reference to this object.
 *
 * Any existing spatial reference
 * is replaced, but under no circumstances does this result in the object
 * being reprojected.  It is just changing the interpretation of the existing
 * geometry.  Note that assigning a spatial reference increments the
 * reference count on the OGRSpatialReference, but does not copy it.
 *
 * Starting with GDAL 2.3, this will also assign the spatial reference to
 * potential sub-geometries of the geometry (OGRGeometryCollection,
 * OGRCurvePolygon/OGRPolygon, OGRCompoundCurve, OGRPolyhedralSurface and their
 * derived classes).
 *
 * This is similar to the SFCOM IGeometry::put_SpatialReference() method.
 *
 * This function is the same as the CPP method
 * OGRGeometry::assignSpatialReference.
 *
 * @param hGeom handle on the geometry to apply the new spatial reference
 * system.
 * @param hSRS handle on the new spatial reference system to apply.
 */

void OGR_G_AssignSpatialReference(OGRGeometryH hGeom, OGRSpatialReferenceH hSRS)

{
    VALIDATE_POINTER0(hGeom, "OGR_G_AssignSpatialReference");

    OGRGeometry::FromHandle(hGeom)->assignSpatialReference(
        OGRSpatialReference::FromHandle(hSRS));
}

/************************************************************************/
/*                             Intersects()                             */
/************************************************************************/

/**
 * \brief Do these features intersect?
 *
 * Determines whether two geometries intersect.  If GEOS is enabled, then
 * this is done in rigorous fashion otherwise TRUE is returned if the
 * envelopes (bounding boxes) of the two geometries overlap.
 *
 * The poOtherGeom argument may be safely NULL, but in this case the method
 * will always return TRUE.   That is, a NULL geometry is treated as being
 * everywhere.
 *
 * This method is the same as the C function OGR_G_Intersects().
 *
 * @param poOtherGeom the other geometry to test against.
 *
 * @return TRUE if the geometries intersect, otherwise FALSE.
 */

OGRBoolean OGRGeometry::Intersects(const OGRGeometry *poOtherGeom) const

{
    if (poOtherGeom == nullptr)
        return TRUE;

    OGREnvelope oEnv1;
    getEnvelope(&oEnv1);

    OGREnvelope oEnv2;
    poOtherGeom->getEnvelope(&oEnv2);

    if (oEnv1.MaxX < oEnv2.MinX || oEnv1.MaxY < oEnv2.MinY ||
        oEnv2.MaxX < oEnv1.MinX || oEnv2.MaxY < oEnv1.MinY)
        return FALSE;

#ifndef HAVE_GEOS
    // Without GEOS we assume that envelope overlap is equivalent to
    // actual intersection.
    return TRUE;
#else

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hThisGeosGeom = exportToGEOS(hGEOSCtxt);
    GEOSGeom hOtherGeosGeom = poOtherGeom->exportToGEOS(hGEOSCtxt);

    OGRBoolean bResult = FALSE;
    if (hThisGeosGeom != nullptr && hOtherGeosGeom != nullptr)
    {
        bResult =
            GEOSIntersects_r(hGEOSCtxt, hThisGeosGeom, hOtherGeosGeom) != 0;
    }

    GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);
    GEOSGeom_destroy_r(hGEOSCtxt, hOtherGeosGeom);
    freeGEOSContext(hGEOSCtxt);

    return bResult;
#endif  // HAVE_GEOS
}

// Old API compatibility function.

//! @cond Doxygen_Suppress
OGRBoolean OGRGeometry::Intersect(OGRGeometry *poOtherGeom) const

{
    return Intersects(poOtherGeom);
}

//! @endcond

/************************************************************************/
/*                          OGR_G_Intersects()                          */
/************************************************************************/
/**
 * \brief Do these features intersect?
 *
 * Determines whether two geometries intersect.  If GEOS is enabled, then
 * this is done in rigorous fashion otherwise TRUE is returned if the
 * envelopes (bounding boxes) of the two geometries overlap.
 *
 * This function is the same as the CPP method OGRGeometry::Intersects.
 *
 * @param hGeom handle on the first geometry.
 * @param hOtherGeom handle on the other geometry to test against.
 *
 * @return TRUE if the geometries intersect, otherwise FALSE.
 */

int OGR_G_Intersects(OGRGeometryH hGeom, OGRGeometryH hOtherGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_Intersects", FALSE);
    VALIDATE_POINTER1(hOtherGeom, "OGR_G_Intersects", FALSE);

    return OGRGeometry::FromHandle(hGeom)->Intersects(
        OGRGeometry::FromHandle(hOtherGeom));
}

//! @cond Doxygen_Suppress
int OGR_G_Intersect(OGRGeometryH hGeom, OGRGeometryH hOtherGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_Intersect", FALSE);
    VALIDATE_POINTER1(hOtherGeom, "OGR_G_Intersect", FALSE);

    return OGRGeometry::FromHandle(hGeom)->Intersects(
        OGRGeometry::FromHandle(hOtherGeom));
}

//! @endcond

/************************************************************************/
/*                            transformTo()                             */
/************************************************************************/

/**
 * \brief Transform geometry to new spatial reference system.
 *
 * This method will transform the coordinates of a geometry from
 * their current spatial reference system to a new target spatial
 * reference system.  Normally this means reprojecting the vectors,
 * but it could include datum shifts, and changes of units.
 *
 * This method will only work if the geometry already has an assigned
 * spatial reference system, and if it is transformable to the target
 * coordinate system.
 *
 * Because this method requires internal creation and initialization of an
 * OGRCoordinateTransformation object it is significantly more expensive to
 * use this method to transform many geometries than it is to create the
 * OGRCoordinateTransformation in advance, and call transform() with that
 * transformation.  This method exists primarily for convenience when only
 * transforming a single geometry.
 *
 * This method is the same as the C function OGR_G_TransformTo().
 *
 * @param poSR spatial reference system to transform to.
 *
 * @return OGRERR_NONE on success, or an error code.
 */

OGRErr OGRGeometry::transformTo(const OGRSpatialReference *poSR)

{
    if (getSpatialReference() == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Geometry has no SRS");
        return OGRERR_FAILURE;
    }

    if (poSR == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Target SRS is NULL");
        return OGRERR_FAILURE;
    }

    OGRCoordinateTransformation *poCT =
        OGRCreateCoordinateTransformation(getSpatialReference(), poSR);
    if (poCT == nullptr)
        return OGRERR_FAILURE;

    const OGRErr eErr = transform(poCT);

    delete poCT;

    return eErr;
}

/************************************************************************/
/*                         OGR_G_TransformTo()                          */
/************************************************************************/
/**
 * \brief Transform geometry to new spatial reference system.
 *
 * This function will transform the coordinates of a geometry from
 * their current spatial reference system to a new target spatial
 * reference system.  Normally this means reprojecting the vectors,
 * but it could include datum shifts, and changes of units.
 *
 * This function will only work if the geometry already has an assigned
 * spatial reference system, and if it is transformable to the target
 * coordinate system.
 *
 * Because this function requires internal creation and initialization of an
 * OGRCoordinateTransformation object it is significantly more expensive to
 * use this function to transform many geometries than it is to create the
 * OGRCoordinateTransformation in advance, and call transform() with that
 * transformation.  This function exists primarily for convenience when only
 * transforming a single geometry.
 *
 * This function is the same as the CPP method OGRGeometry::transformTo.
 *
 * @param hGeom handle on the geometry to apply the transform to.
 * @param hSRS handle on the spatial reference system to apply.
 *
 * @return OGRERR_NONE on success, or an error code.
 */

OGRErr OGR_G_TransformTo(OGRGeometryH hGeom, OGRSpatialReferenceH hSRS)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_TransformTo", OGRERR_FAILURE);

    return OGRGeometry::FromHandle(hGeom)->transformTo(
        OGRSpatialReference::FromHandle(hSRS));
}

/**
 * \fn OGRErr OGRGeometry::transform( OGRCoordinateTransformation *poCT );
 *
 * \brief Apply arbitrary coordinate transformation to geometry.
 *
 * This method will transform the coordinates of a geometry from
 * their current spatial reference system to a new target spatial
 * reference system.  Normally this means reprojecting the vectors,
 * but it could include datum shifts, and changes of units.
 *
 * Note that this method does not require that the geometry already
 * have a spatial reference system.  It will be assumed that they can
 * be treated as having the source spatial reference system of the
 * OGRCoordinateTransformation object, and the actual SRS of the geometry
 * will be ignored.  On successful completion the output OGRSpatialReference
 * of the OGRCoordinateTransformation will be assigned to the geometry.
 *
 * This method only does reprojection on a point-by-point basis. It does not
 * include advanced logic to deal with discontinuities at poles or antimeridian.
 * For that, use the OGRGeometryFactory::transformWithOptions() method.
 *
 * This method is the same as the C function OGR_G_Transform().
 *
 * @param poCT the transformation to apply.
 *
 * @return OGRERR_NONE on success or an error code.
 */

/************************************************************************/
/*                          OGR_G_Transform()                           */
/************************************************************************/
/**
 * \brief Apply arbitrary coordinate transformation to geometry.
 *
 * This function will transform the coordinates of a geometry from
 * their current spatial reference system to a new target spatial
 * reference system.  Normally this means reprojecting the vectors,
 * but it could include datum shifts, and changes of units.
 *
 * Note that this function does not require that the geometry already
 * have a spatial reference system.  It will be assumed that they can
 * be treated as having the source spatial reference system of the
 * OGRCoordinateTransformation object, and the actual SRS of the geometry
 * will be ignored.  On successful completion the output OGRSpatialReference
 * of the OGRCoordinateTransformation will be assigned to the geometry.
 *
 * This function only does reprojection on a point-by-point basis. It does not
 * include advanced logic to deal with discontinuities at poles or antimeridian.
 * For that, use the OGR_GeomTransformer_Create() and
 * OGR_GeomTransformer_Transform() functions.
 *
 * This function is the same as the CPP method OGRGeometry::transform.
 *
 * @param hGeom handle on the geometry to apply the transform to.
 * @param hTransform handle on the transformation to apply.
 *
 * @return OGRERR_NONE on success or an error code.
 */

OGRErr OGR_G_Transform(OGRGeometryH hGeom,
                       OGRCoordinateTransformationH hTransform)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_Transform", OGRERR_FAILURE);

    return OGRGeometry::FromHandle(hGeom)->transform(
        OGRCoordinateTransformation::FromHandle(hTransform));
}

/**
 * \fn int OGRGeometry::getDimension() const;
 *
 * \brief Get the dimension of this object.
 *
 * This method corresponds to the SFCOM IGeometry::GetDimension() method.
 * It indicates the dimension of the object, but does not indicate the
 * dimension of the underlying space (as indicated by
 * OGRGeometry::getCoordinateDimension()).
 *
 * This method is the same as the C function OGR_G_GetDimension().
 *
 * @return 0 for points, 1 for lines and 2 for surfaces.
 */

/**
 * \brief Get the geometry type that conforms with ISO SQL/MM Part3
 *
 * @return the geometry type that conforms with ISO SQL/MM Part3
 */
OGRwkbGeometryType OGRGeometry::getIsoGeometryType() const
{
    OGRwkbGeometryType nGType = wkbFlatten(getGeometryType());

    if (flags & OGR_G_3D)
        nGType = static_cast<OGRwkbGeometryType>(nGType + 1000);
    if (flags & OGR_G_MEASURED)
        nGType = static_cast<OGRwkbGeometryType>(nGType + 2000);

    return nGType;
}

/************************************************************************/
/*                  OGRGeometry::segmentize()                           */
/************************************************************************/
/**
 *
 * \brief Modify the geometry such it has no segment longer then the
 * given distance.
 *
 * This method modifies the geometry to add intermediate vertices if necessary
 * so that the maximum length between 2 consecutive vertices is lower than
 * dfMaxLength.
 *
 * Interpolated points will have Z and M values (if needed) set to 0.
 * Distance computation is performed in 2d only
 *
 * This function is the same as the C function OGR_G_Segmentize()
 *
 * @param dfMaxLength the maximum distance between 2 points after segmentization
 * @return (since 3.10) true in case of success, false in case of error.
 */

bool OGRGeometry::segmentize(CPL_UNUSED double dfMaxLength)
{
    // Do nothing.
    return true;
}

/************************************************************************/
/*                         OGR_G_Segmentize()                           */
/************************************************************************/

/**
 *
 * \brief Modify the geometry such it has no segment longer then the given
 * distance.
 *
 * Interpolated points will have Z and M values (if needed) set to 0.
 * Distance computation is performed in 2d only.
 *
 * This function is the same as the CPP method OGRGeometry::segmentize().
 *
 * @param hGeom handle on the geometry to segmentize
 * @param dfMaxLength the maximum distance between 2 points after segmentization
 */

void CPL_DLL OGR_G_Segmentize(OGRGeometryH hGeom, double dfMaxLength)
{
    VALIDATE_POINTER0(hGeom, "OGR_G_Segmentize");

    if (dfMaxLength <= 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "dfMaxLength must be strictly positive");
        return;
    }
    OGRGeometry::FromHandle(hGeom)->segmentize(dfMaxLength);
}

/************************************************************************/
/*                         OGR_G_GetDimension()                         */
/************************************************************************/
/**
 *
 * \brief Get the dimension of this geometry.
 *
 * This function corresponds to the SFCOM IGeometry::GetDimension() method.
 * It indicates the dimension of the geometry, but does not indicate the
 * dimension of the underlying space (as indicated by
 * OGR_G_GetCoordinateDimension() function).
 *
 * This function is the same as the CPP method OGRGeometry::getDimension().
 *
 * @param hGeom handle on the geometry to get the dimension from.
 * @return 0 for points, 1 for lines and 2 for surfaces.
 */

int OGR_G_GetDimension(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_GetDimension", 0);

    return OGRGeometry::FromHandle(hGeom)->getDimension();
}

/************************************************************************/
/*                       getCoordinateDimension()                       */
/************************************************************************/
/**
 * \brief Get the dimension of the coordinates in this object.
 *
 * This method is the same as the C function OGR_G_GetCoordinateDimension().
 *
 * @deprecated use CoordinateDimension().
 *
 * @return this will return 2 or 3.
 */

int OGRGeometry::getCoordinateDimension() const

{
    return (flags & OGR_G_3D) ? 3 : 2;
}

/************************************************************************/
/*                        CoordinateDimension()                         */
/************************************************************************/
/**
 * \brief Get the dimension of the coordinates in this object.
 *
 * This method is the same as the C function OGR_G_CoordinateDimension().
 *
 * @return this will return 2 for XY, 3 for XYZ and XYM, and 4 for XYZM data.
 *
 * @since GDAL 2.1
 */

int OGRGeometry::CoordinateDimension() const

{
    if ((flags & OGR_G_3D) && (flags & OGR_G_MEASURED))
        return 4;
    else if ((flags & OGR_G_3D) || (flags & OGR_G_MEASURED))
        return 3;
    else
        return 2;
}

/************************************************************************/
/*                    OGR_G_GetCoordinateDimension()                    */
/************************************************************************/
/**
 *
 * \brief Get the dimension of the coordinates in this geometry.
 *
 * This function is the same as the CPP method
 * OGRGeometry::getCoordinateDimension().
 *
 * @param hGeom handle on the geometry to get the dimension of the
 * coordinates from.
 *
 * @deprecated use OGR_G_CoordinateDimension(), OGR_G_Is3D(), or
 * OGR_G_IsMeasured().
 *
 * @return this will return 2 or 3.
 */

int OGR_G_GetCoordinateDimension(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_GetCoordinateDimension", 0);

    return OGRGeometry::FromHandle(hGeom)->getCoordinateDimension();
}

/************************************************************************/
/*                    OGR_G_CoordinateDimension()                       */
/************************************************************************/
/**
 *
 * \brief Get the dimension of the coordinates in this geometry.
 *
 * This function is the same as the CPP method
 * OGRGeometry::CoordinateDimension().
 *
 * @param hGeom handle on the geometry to get the dimension of the
 * coordinates from.
 *
 * @return this will return 2 for XY, 3 for XYZ and XYM, and 4 for XYZM data.
 *
 * @since GDAL 2.1
 */

int OGR_G_CoordinateDimension(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_CoordinateDimension", 0);

    return OGRGeometry::FromHandle(hGeom)->CoordinateDimension();
}

/**
 *
 * \brief See whether this geometry has Z coordinates.
 *
 * This function is the same as the CPP method
 * OGRGeometry::Is3D().
 *
 * @param hGeom handle on the geometry to check whether it has Z coordinates.
 *
 * @return TRUE if the geometry has Z coordinates.
 * @since GDAL 2.1
 */

int OGR_G_Is3D(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_Is3D", 0);

    return OGRGeometry::FromHandle(hGeom)->Is3D();
}

/**
 *
 * \brief See whether this geometry is measured.
 *
 * This function is the same as the CPP method
 * OGRGeometry::IsMeasured().
 *
 * @param hGeom handle on the geometry to check whether it is measured.
 *
 * @return TRUE if the geometry has M coordinates.
 * @since GDAL 2.1
 */

int OGR_G_IsMeasured(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_IsMeasured", 0);

    return OGRGeometry::FromHandle(hGeom)->IsMeasured();
}

/************************************************************************/
/*                       setCoordinateDimension()                       */
/************************************************************************/

/**
 * \brief Set the coordinate dimension.
 *
 * This method sets the explicit coordinate dimension.  Setting the coordinate
 * dimension of a geometry to 2 should zero out any existing Z values.  Setting
 * the dimension of a geometry collection, a compound curve, a polygon, etc.
 * will affect the children geometries.
 * This will also remove the M dimension if present before this call.
 *
 * @deprecated use set3D() or setMeasured().
 *
 * @param nNewDimension New coordinate dimension value, either 2 or 3.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRGeometry::setCoordinateDimension(int nNewDimension)

{
    if (nNewDimension == 2)
        flags &= ~OGR_G_3D;
    else
        flags |= OGR_G_3D;
    return setMeasured(FALSE);
}

/**
 * \brief Add or remove the Z coordinate dimension.
 *
 * This method adds or removes the explicit Z coordinate dimension.
 * Removing the Z coordinate dimension of a geometry will remove any
 * existing Z values.  Adding the Z dimension to a geometry
 * collection, a compound curve, a polygon, etc.  will affect the
 * children geometries.
 *
 * @param bIs3D Should the geometry have a Z dimension, either TRUE or FALSE.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 * @since GDAL 2.1
 */

bool OGRGeometry::set3D(OGRBoolean bIs3D)

{
    if (bIs3D)
        flags |= OGR_G_3D;
    else
        flags &= ~OGR_G_3D;
    return true;
}

/**
 * \brief Add or remove the M coordinate dimension.
 *
 * This method adds or removes the explicit M coordinate dimension.
 * Removing the M coordinate dimension of a geometry will remove any
 * existing M values.  Adding the M dimension to a geometry
 * collection, a compound curve, a polygon, etc.  will affect the
 * children geometries.
 *
 * @param bIsMeasured Should the geometry have a M dimension, either
 * TRUE or FALSE.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 * @since GDAL 2.1
 */

bool OGRGeometry::setMeasured(OGRBoolean bIsMeasured)

{
    if (bIsMeasured)
        flags |= OGR_G_MEASURED;
    else
        flags &= ~OGR_G_MEASURED;
    return true;
}

/************************************************************************/
/*                    OGR_G_SetCoordinateDimension()                    */
/************************************************************************/

/**
 * \brief Set the coordinate dimension.
 *
 * This method sets the explicit coordinate dimension.  Setting the coordinate
 * dimension of a geometry to 2 should zero out any existing Z values. Setting
 * the dimension of a geometry collection, a compound curve, a polygon, etc.
 * will affect the children geometries.
 * This will also remove the M dimension if present before this call.
 *
 * @deprecated use OGR_G_Set3D() or OGR_G_SetMeasured().
 *
 * @param hGeom handle on the geometry to set the dimension of the
 * coordinates.
 * @param nNewDimension New coordinate dimension value, either 2 or 3.
 */

void OGR_G_SetCoordinateDimension(OGRGeometryH hGeom, int nNewDimension)

{
    VALIDATE_POINTER0(hGeom, "OGR_G_SetCoordinateDimension");

    OGRGeometry::FromHandle(hGeom)->setCoordinateDimension(nNewDimension);
}

/************************************************************************/
/*                    OGR_G_Set3D()                                     */
/************************************************************************/

/**
 * \brief Add or remove the Z coordinate dimension.
 *
 * This method adds or removes the explicit Z coordinate dimension.
 * Removing the Z coordinate dimension of a geometry will remove any
 * existing Z values.  Adding the Z dimension to a geometry
 * collection, a compound curve, a polygon, etc.  will affect the
 * children geometries.
 *
 * @param hGeom handle on the geometry to set or unset the Z dimension.
 * @param bIs3D Should the geometry have a Z dimension, either TRUE or FALSE.
 * @since GDAL 2.1
 */

void OGR_G_Set3D(OGRGeometryH hGeom, int bIs3D)

{
    VALIDATE_POINTER0(hGeom, "OGR_G_Set3D");

    OGRGeometry::FromHandle(hGeom)->set3D(bIs3D);
}

/************************************************************************/
/*                    OGR_G_SetMeasured()                               */
/************************************************************************/

/**
 * \brief Add or remove the M coordinate dimension.
 *
 * This method adds or removes the explicit M coordinate dimension.
 * Removing the M coordinate dimension of a geometry will remove any
 * existing M values.  Adding the M dimension to a geometry
 * collection, a compound curve, a polygon, etc.  will affect the
 * children geometries.
 *
 * @param hGeom handle on the geometry to set or unset the M dimension.
 * @param bIsMeasured Should the geometry have a M dimension, either
 * TRUE or FALSE.
 * @since GDAL 2.1
 */

void OGR_G_SetMeasured(OGRGeometryH hGeom, int bIsMeasured)

{
    VALIDATE_POINTER0(hGeom, "OGR_G_SetMeasured");

    OGRGeometry::FromHandle(hGeom)->setMeasured(bIsMeasured);
}

/**
 * \fn int OGRGeometry::Equals( OGRGeometry *poOtherGeom ) const;
 *
 * \brief Returns TRUE if two geometries are equivalent.
 *
 * This operation implements the SQL/MM ST_OrderingEquals() operation.
 *
 * The comparison is done in a structural way, that is to say that the geometry
 * types must be identical, as well as the number and ordering of sub-geometries
 * and vertices.
 * Or equivalently, two geometries are considered equal by this method if their
 * WKT/WKB representation is equal.
 * Note: this must be distinguished for equality in a spatial way (which is
 * the purpose of the ST_Equals() operation).
 *
 * This method is the same as the C function OGR_G_Equals().
 *
 * @return TRUE if equivalent or FALSE otherwise.
 */

// Backward compatibility method.

//! @cond Doxygen_Suppress
int OGRGeometry::Equal(OGRGeometry *poOtherGeom) const
{
    return Equals(poOtherGeom);
}

//! @endcond

/************************************************************************/
/*                            OGR_G_Equals()                            */
/************************************************************************/

/**
 * \brief Returns TRUE if two geometries are equivalent.
 *
 * This operation implements the SQL/MM ST_OrderingEquals() operation.
 *
 * The comparison is done in a structural way, that is to say that the geometry
 * types must be identical, as well as the number and ordering of sub-geometries
 * and vertices.
 * Or equivalently, two geometries are considered equal by this method if their
 * WKT/WKB representation is equal.
 * Note: this must be distinguished for equality in a spatial way (which is
 * the purpose of the ST_Equals() operation).
 *
 * This function is the same as the CPP method OGRGeometry::Equals() method.
 *
 * @param hGeom handle on the first geometry.
 * @param hOther handle on the other geometry to test against.
 * @return TRUE if equivalent or FALSE otherwise.
 */

int OGR_G_Equals(OGRGeometryH hGeom, OGRGeometryH hOther)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_Equals", FALSE);

    if (hOther == nullptr)
    {
        CPLError(CE_Failure, CPLE_ObjectNull,
                 "hOther was NULL in OGR_G_Equals");
        return 0;
    }

    return OGRGeometry::FromHandle(hGeom)->Equals(
        OGRGeometry::FromHandle(hOther));
}

//! @cond Doxygen_Suppress
int OGR_G_Equal(OGRGeometryH hGeom, OGRGeometryH hOther)

{
    if (hGeom == nullptr)
    {
        CPLError(CE_Failure, CPLE_ObjectNull, "hGeom was NULL in OGR_G_Equal");
        return 0;
    }

    if (hOther == nullptr)
    {
        CPLError(CE_Failure, CPLE_ObjectNull, "hOther was NULL in OGR_G_Equal");
        return 0;
    }

    return OGRGeometry::FromHandle(hGeom)->Equals(
        OGRGeometry::FromHandle(hOther));
}

//! @endcond

/**
 * \fn int OGRGeometry::WkbSize() const;
 *
 * \brief Returns size of related binary representation.
 *
 * This method returns the exact number of bytes required to hold the
 * well known binary representation of this geometry object.  Its computation
 * may be slightly expensive for complex geometries.
 *
 * This method relates to the SFCOM IWks::WkbSize() method.
 *
 * This method is the same as the C function OGR_G_WkbSize().
 *
 * @return size of binary representation in bytes.
 */

/************************************************************************/
/*                           OGR_G_WkbSize()                            */
/************************************************************************/
/**
 * \brief Returns size of related binary representation.
 *
 * This function returns the exact number of bytes required to hold the
 * well known binary representation of this geometry object.  Its computation
 * may be slightly expensive for complex geometries.
 *
 * This function relates to the SFCOM IWks::WkbSize() method.
 *
 * This function is the same as the CPP method OGRGeometry::WkbSize().
 *
 * Use OGR_G_WkbSizeEx() if called on huge geometries (> 2 GB serialized)
 *
 * @param hGeom handle on the geometry to get the binary size from.
 * @return size of binary representation in bytes.
 */

int OGR_G_WkbSize(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_WkbSize", 0);

    const size_t nSize = OGRGeometry::FromHandle(hGeom)->WkbSize();
    if (nSize > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "OGR_G_WkbSize() would return a value beyond int range. "
                 "Use OGR_G_WkbSizeEx() instead");
        return 0;
    }
    return static_cast<int>(nSize);
}

/************************************************************************/
/*                         OGR_G_WkbSizeEx()                            */
/************************************************************************/
/**
 * \brief Returns size of related binary representation.
 *
 * This function returns the exact number of bytes required to hold the
 * well known binary representation of this geometry object.  Its computation
 * may be slightly expensive for complex geometries.
 *
 * This function relates to the SFCOM IWks::WkbSize() method.
 *
 * This function is the same as the CPP method OGRGeometry::WkbSize().
 *
 * @param hGeom handle on the geometry to get the binary size from.
 * @return size of binary representation in bytes.
 * @since GDAL 3.3
 */

size_t OGR_G_WkbSizeEx(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_WkbSizeEx", 0);

    return OGRGeometry::FromHandle(hGeom)->WkbSize();
}

/**
 * \fn void OGRGeometry::getEnvelope(OGREnvelope *psEnvelope) const;
 *
 * \brief Computes and returns the bounding envelope for this geometry
 * in the passed psEnvelope structure.
 *
 * This method is the same as the C function OGR_G_GetEnvelope().
 *
 * @param psEnvelope the structure in which to place the results.
 */

/************************************************************************/
/*                         OGR_G_GetEnvelope()                          */
/************************************************************************/
/**
 * \brief Computes and returns the bounding envelope for this geometry
 * in the passed psEnvelope structure.
 *
 * This function is the same as the CPP method OGRGeometry::getEnvelope().
 *
 * @param hGeom handle of the geometry to get envelope from.
 * @param psEnvelope the structure in which to place the results.
 */

void OGR_G_GetEnvelope(OGRGeometryH hGeom, OGREnvelope *psEnvelope)

{
    VALIDATE_POINTER0(hGeom, "OGR_G_GetEnvelope");

    OGRGeometry::FromHandle(hGeom)->getEnvelope(psEnvelope);
}

/**
 * \fn void OGRGeometry::getEnvelope(OGREnvelope3D *psEnvelope) const;
 *
 * \brief Computes and returns the bounding envelope (3D) for this
 * geometry in the passed psEnvelope structure.
 *
 * This method is the same as the C function OGR_G_GetEnvelope3D().
 *
 * @param psEnvelope the structure in which to place the results.
 *
 * @since OGR 1.9.0
 */

/************************************************************************/
/*                        OGR_G_GetEnvelope3D()                         */
/************************************************************************/
/**
 * \brief Computes and returns the bounding envelope (3D) for this
 * geometry in the passed psEnvelope structure.
 *
 * This function is the same as the CPP method OGRGeometry::getEnvelope().
 *
 * @param hGeom handle of the geometry to get envelope from.
 * @param psEnvelope the structure in which to place the results.
 *
 * @since OGR 1.9.0
 */

void OGR_G_GetEnvelope3D(OGRGeometryH hGeom, OGREnvelope3D *psEnvelope)

{
    VALIDATE_POINTER0(hGeom, "OGR_G_GetEnvelope3D");

    OGRGeometry::FromHandle(hGeom)->getEnvelope(psEnvelope);
}

/************************************************************************/
/*                        importFromWkb()                               */
/************************************************************************/

/**
 * \brief Assign geometry from well known binary data.
 *
 * The object must have already been instantiated as the correct derived
 * type of geometry object to match the binaries type.  This method is used
 * by the OGRGeometryFactory class, but not normally called by application
 * code.
 *
 * This method relates to the SFCOM IWks::ImportFromWKB() method.
 *
 * This method is the same as the C function OGR_G_ImportFromWkb().
 *
 * @param pabyData the binary input data.
 * @param nSize the size of pabyData in bytes, or -1 if not known.
 * @param eWkbVariant if wkbVariantPostGIS1, special interpretation is
 * done for curve geometries code
 *
 * @return OGRERR_NONE if all goes well, otherwise any of
 * OGRERR_NOT_ENOUGH_DATA, OGRERR_UNSUPPORTED_GEOMETRY_TYPE, or
 * OGRERR_CORRUPT_DATA may be returned.
 */

OGRErr OGRGeometry::importFromWkb(const GByte *pabyData, size_t nSize,
                                  OGRwkbVariant eWkbVariant)
{
    size_t nBytesConsumedOutIgnored = 0;
    return importFromWkb(pabyData, nSize, eWkbVariant,
                         nBytesConsumedOutIgnored);
}

/**
 * \fn OGRErr OGRGeometry::importFromWkb( const unsigned char * pabyData,
 * size_t nSize, OGRwkbVariant eWkbVariant, size_t& nBytesConsumedOut );
 *
 * \brief Assign geometry from well known binary data.
 *
 * The object must have already been instantiated as the correct derived
 * type of geometry object to match the binaries type.  This method is used
 * by the OGRGeometryFactory class, but not normally called by application
 * code.
 *
 * This method relates to the SFCOM IWks::ImportFromWKB() method.
 *
 * This method is the same as the C function OGR_G_ImportFromWkb().
 *
 * @param pabyData the binary input data.
 * @param nSize the size of pabyData in bytes, or -1 if not known.
 * @param eWkbVariant if wkbVariantPostGIS1, special interpretation is
 * done for curve geometries code
 * @param nBytesConsumedOut output parameter. Number of bytes consumed.
 *
 * @return OGRERR_NONE if all goes well, otherwise any of
 * OGRERR_NOT_ENOUGH_DATA, OGRERR_UNSUPPORTED_GEOMETRY_TYPE, or
 * OGRERR_CORRUPT_DATA may be returned.
 *
 * @since GDAL 2.3
 */

/************************************************************************/
/*                        OGR_G_ImportFromWkb()                         */
/************************************************************************/
/**
 * \brief Assign geometry from well known binary data.
 *
 * The object must have already been instantiated as the correct derived
 * type of geometry object to match the binaries type.
 *
 * This function relates to the SFCOM IWks::ImportFromWKB() method.
 *
 * This function is the same as the CPP method OGRGeometry::importFromWkb().
 *
 * @param hGeom handle on the geometry to assign the well know binary data to.
 * @param pabyData the binary input data.
 * @param nSize the size of pabyData in bytes, or -1 if not known.
 *
 * @return OGRERR_NONE if all goes well, otherwise any of
 * OGRERR_NOT_ENOUGH_DATA, OGRERR_UNSUPPORTED_GEOMETRY_TYPE, or
 * OGRERR_CORRUPT_DATA may be returned.
 */

OGRErr OGR_G_ImportFromWkb(OGRGeometryH hGeom, const void *pabyData, int nSize)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_ImportFromWkb", OGRERR_FAILURE);

    return OGRGeometry::FromHandle(hGeom)->importFromWkb(
        static_cast<const GByte *>(pabyData), nSize);
}

/************************************************************************/
/*                       OGRGeometry::exportToWkb()                     */
/************************************************************************/

/* clang-format off */
/**
 * \brief Convert a geometry into well known binary format.
 *
 * This method relates to the SFCOM IWks::ExportToWKB() method.
 *
 * This method is the same as the C function OGR_G_ExportToWkb() or
 * OGR_G_ExportToIsoWkb(), depending on the value of eWkbVariant.
 *
 * @param eByteOrder One of wkbXDR or wkbNDR indicating MSB or LSB byte order
 *               respectively.
 * @param pabyData a buffer into which the binary representation is
 *                      written.  This buffer must be at least
 *                      OGRGeometry::WkbSize() byte in size.
 * @param eWkbVariant What standard to use when exporting geometries
 *                      with three dimensions (or more). The default
 *                      wkbVariantOldOgc is the historical OGR
 *                      variant. wkbVariantIso is the variant defined
 *                      in ISO SQL/MM and adopted by OGC for SFSQL
 *                      1.2.
 *
 * @return Currently OGRERR_NONE is always returned.
 */
/* clang-format on */
OGRErr OGRGeometry::exportToWkb(OGRwkbByteOrder eByteOrder,
                                unsigned char *pabyData,
                                OGRwkbVariant eWkbVariant) const
{
    OGRwkbExportOptions sOptions;
    sOptions.eByteOrder = eByteOrder;
    sOptions.eWkbVariant = eWkbVariant;
    return exportToWkb(pabyData, &sOptions);
}

/************************************************************************/
/*                         OGR_G_ExportToWkb()                          */
/************************************************************************/
/**
 * \brief Convert a geometry well known binary format
 *
 * This function relates to the SFCOM IWks::ExportToWKB() method.
 *
 * For backward compatibility purposes, it exports the Old-style 99-402
 * extended dimension (Z) WKB types for types Point, LineString, Polygon,
 * MultiPoint, MultiLineString, MultiPolygon and GeometryCollection.
 * For other geometry types, it is equivalent to OGR_G_ExportToIsoWkb().
 *
 * This function is the same as the CPP method
 * OGRGeometry::exportToWkb(OGRwkbByteOrder, unsigned char *,
 * OGRwkbVariant) with eWkbVariant = wkbVariantOldOgc.
 *
 * @param hGeom handle on the geometry to convert to a well know binary
 * data from.
 * @param eOrder One of wkbXDR or wkbNDR indicating MSB or LSB byte order
 *               respectively.
 * @param pabyDstBuffer a buffer into which the binary representation is
 *                      written.  This buffer must be at least
 *                      OGR_G_WkbSize() byte in size.
 *
 * @return Currently OGRERR_NONE is always returned.
 */

OGRErr OGR_G_ExportToWkb(OGRGeometryH hGeom, OGRwkbByteOrder eOrder,
                         unsigned char *pabyDstBuffer)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_ExportToWkb", OGRERR_FAILURE);

    return OGRGeometry::FromHandle(hGeom)->exportToWkb(eOrder, pabyDstBuffer);
}

/************************************************************************/
/*                        OGR_G_ExportToIsoWkb()                        */
/************************************************************************/
/**
 * \brief Convert a geometry into SFSQL 1.2 / ISO SQL/MM Part 3 well known
 * binary format
 *
 * This function relates to the SFCOM IWks::ExportToWKB() method.
 * It exports the SFSQL 1.2 and ISO SQL/MM Part 3 extended dimension (Z&M) WKB
 * types.
 *
 * This function is the same as the CPP method
 * OGRGeometry::exportToWkb(OGRwkbByteOrder, unsigned char *, OGRwkbVariant)
 * with eWkbVariant = wkbVariantIso.
 *
 * @param hGeom handle on the geometry to convert to a well know binary
 * data from.
 * @param eOrder One of wkbXDR or wkbNDR indicating MSB or LSB byte order
 *               respectively.
 * @param pabyDstBuffer a buffer into which the binary representation is
 *                      written.  This buffer must be at least
 *                      OGR_G_WkbSize() byte in size.
 *
 * @return Currently OGRERR_NONE is always returned.
 *
 * @since GDAL 2.0
 */

OGRErr OGR_G_ExportToIsoWkb(OGRGeometryH hGeom, OGRwkbByteOrder eOrder,
                            unsigned char *pabyDstBuffer)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_ExportToIsoWkb", OGRERR_FAILURE);

    return OGRGeometry::FromHandle(hGeom)->exportToWkb(eOrder, pabyDstBuffer,
                                                       wkbVariantIso);
}

/************************************************************************/
/*                        OGR_G_ExportToWkbEx()                         */
/************************************************************************/

/* clang-format off */
/**
 * \fn OGRErr OGRGeometry::exportToWkb(unsigned char *pabyDstBuffer, const OGRwkbExportOptions *psOptions=nullptr) const
 *
 * \brief Convert a geometry into well known binary format
 *
 * This function relates to the SFCOM IWks::ExportToWKB() method.
 *
 * This function is the same as the C function OGR_G_ExportToWkbEx().
 *
 * @param pabyDstBuffer a buffer into which the binary representation is
 *                      written.  This buffer must be at least
 *                      OGR_G_WkbSize() byte in size.
 * @param psOptions WKB export options.

 * @return Currently OGRERR_NONE is always returned.
 *
 * @since GDAL 3.9
 */
/* clang-format on */

/**
 * \brief Convert a geometry into well known binary format
 *
 * This function relates to the SFCOM IWks::ExportToWKB() method.
 *
 * This function is the same as the CPP method
 * OGRGeometry::exportToWkb(unsigned char *, const OGRwkbExportOptions*)
 *
 * @param hGeom handle on the geometry to convert to a well know binary
 * data from.
 * @param pabyDstBuffer a buffer into which the binary representation is
 *                      written.  This buffer must be at least
 *                      OGR_G_WkbSize() byte in size.
 * @param psOptions WKB export options.

 * @return Currently OGRERR_NONE is always returned.
 *
 * @since GDAL 3.9
 */

OGRErr OGR_G_ExportToWkbEx(OGRGeometryH hGeom, unsigned char *pabyDstBuffer,
                           const OGRwkbExportOptions *psOptions)
{
    VALIDATE_POINTER1(hGeom, "OGR_G_ExportToWkbEx", OGRERR_FAILURE);

    return OGRGeometry::FromHandle(hGeom)->exportToWkb(pabyDstBuffer,
                                                       psOptions);
}

/**
 * \fn OGRErr OGRGeometry::importFromWkt( const char ** ppszInput );
 *
 * \brief Assign geometry from well known text data.
 *
 * The object must have already been instantiated as the correct derived
 * type of geometry object to match the text type.  This method is used
 * by the OGRGeometryFactory class, but not normally called by application
 * code.
 *
 * This method relates to the SFCOM IWks::ImportFromWKT() method.
 *
 * This method is the same as the C function OGR_G_ImportFromWkt().
 *
 * @param ppszInput pointer to a pointer to the source text.  The pointer is
 *                    updated to pointer after the consumed text.
 *
 * @return OGRERR_NONE if all goes well, otherwise any of
 * OGRERR_NOT_ENOUGH_DATA, OGRERR_UNSUPPORTED_GEOMETRY_TYPE, or
 * OGRERR_CORRUPT_DATA may be returned.
 */

/************************************************************************/
/*                        OGR_G_ImportFromWkt()                         */
/************************************************************************/
/**
 * \brief Assign geometry from well known text data.
 *
 * The object must have already been instantiated as the correct derived
 * type of geometry object to match the text type.
 *
 * This function relates to the SFCOM IWks::ImportFromWKT() method.
 *
 * This function is the same as the CPP method OGRGeometry::importFromWkt().
 *
 * @param hGeom handle on the geometry to assign well know text data to.
 * @param ppszSrcText pointer to a pointer to the source text.  The pointer is
 *                    updated to pointer after the consumed text.
 *
 * @return OGRERR_NONE if all goes well, otherwise any of
 * OGRERR_NOT_ENOUGH_DATA, OGRERR_UNSUPPORTED_GEOMETRY_TYPE, or
 * OGRERR_CORRUPT_DATA may be returned.
 */

OGRErr OGR_G_ImportFromWkt(OGRGeometryH hGeom, char **ppszSrcText)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_ImportFromWkt", OGRERR_FAILURE);

    return OGRGeometry::FromHandle(hGeom)->importFromWkt(
        const_cast<const char **>(ppszSrcText));
}

/************************************************************************/
/*                        importPreambleFromWkt()                      */
/************************************************************************/

// Returns -1 if processing must continue.
//! @cond Doxygen_Suppress
OGRErr OGRGeometry::importPreambleFromWkt(const char **ppszInput, int *pbHasZ,
                                          int *pbHasM, bool *pbIsEmpty)
{
    const char *pszInput = *ppszInput;

    /* -------------------------------------------------------------------- */
    /*      Clear existing Geoms.                                           */
    /* -------------------------------------------------------------------- */
    empty();
    *pbIsEmpty = false;

    /* -------------------------------------------------------------------- */
    /*      Read and verify the type keyword, and ensure it matches the     */
    /*      actual type of this container.                                  */
    /* -------------------------------------------------------------------- */
    bool bHasM = false;
    bool bHasZ = false;
    bool bAlreadyGotDimension = false;

    char szToken[OGR_WKT_TOKEN_MAX] = {};
    pszInput = OGRWktReadToken(pszInput, szToken);
    if (szToken[0] != '\0')
    {
        // Postgis EWKT: POINTM instead of POINT M.
        // Current QGIS versions (at least <= 3.38) also export POINTZ.
        const size_t nTokenLen = strlen(szToken);
        if (szToken[nTokenLen - 1] == 'M' || szToken[nTokenLen - 1] == 'm')
        {
            szToken[nTokenLen - 1] = '\0';
            bHasM = true;
            bAlreadyGotDimension = true;

            if (nTokenLen > 2 && (szToken[nTokenLen - 2] == 'Z' ||
                                  szToken[nTokenLen - 2] == 'z'))
            {
                bHasZ = true;
                szToken[nTokenLen - 2] = '\0';
            }
        }
        else if (szToken[nTokenLen - 1] == 'Z' || szToken[nTokenLen - 1] == 'z')
        {
            szToken[nTokenLen - 1] = '\0';
            bHasZ = true;
            bAlreadyGotDimension = true;
        }
    }

    if (!EQUAL(szToken, getGeometryName()))
        return OGRERR_CORRUPT_DATA;

    /* -------------------------------------------------------------------- */
    /*      Check for Z, M or ZM                                            */
    /* -------------------------------------------------------------------- */
    if (!bAlreadyGotDimension)
    {
        const char *pszNewInput = OGRWktReadToken(pszInput, szToken);
        if (EQUAL(szToken, "Z"))
        {
            pszInput = pszNewInput;
            bHasZ = true;
        }
        else if (EQUAL(szToken, "M"))
        {
            pszInput = pszNewInput;
            bHasM = true;
        }
        else if (EQUAL(szToken, "ZM"))
        {
            pszInput = pszNewInput;
            bHasZ = true;
            bHasM = true;
        }
    }
    *pbHasZ = bHasZ;
    *pbHasM = bHasM;

    /* -------------------------------------------------------------------- */
    /*      Check for EMPTY ...                                             */
    /* -------------------------------------------------------------------- */
    const char *pszNewInput = OGRWktReadToken(pszInput, szToken);
    if (EQUAL(szToken, "EMPTY"))
    {
        *ppszInput = pszNewInput;
        *pbIsEmpty = true;
        if (bHasZ)
            set3D(TRUE);
        if (bHasM)
            setMeasured(TRUE);
        return OGRERR_NONE;
    }

    if (!EQUAL(szToken, "("))
        return OGRERR_CORRUPT_DATA;

    if (!bHasZ && !bHasM)
    {
        // Test for old-style XXXXXXXXX(EMPTY).
        pszNewInput = OGRWktReadToken(pszNewInput, szToken);
        if (EQUAL(szToken, "EMPTY"))
        {
            pszNewInput = OGRWktReadToken(pszNewInput, szToken);

            if (EQUAL(szToken, ","))
            {
                // This is OK according to SFSQL SPEC.
            }
            else if (!EQUAL(szToken, ")"))
            {
                return OGRERR_CORRUPT_DATA;
            }
            else
            {
                *ppszInput = pszNewInput;
                empty();
                *pbIsEmpty = true;
                return OGRERR_NONE;
            }
        }
    }

    *ppszInput = pszInput;

    return OGRERR_NONE;
}

//! @endcond

/************************************************************************/
/*                           wktTypeString()                            */
/************************************************************************/

//! @cond Doxygen_Suppress
/** Get a type string for WKT, padded with a space at the end.
 *
 * @param variant  OGR type variant
 * @return  "Z " for 3D, "M " for measured, "ZM " for both, or the empty string.
 */
std::string OGRGeometry::wktTypeString(OGRwkbVariant variant) const
{
    std::string s(" ");

    if (variant == wkbVariantIso)
    {
        if (flags & OGR_G_3D)
            s += "Z";
        if (flags & OGR_G_MEASURED)
            s += "M";
    }
    if (s.size() > 1)
        s += " ";
    return s;
}

//! @endcond

/**
 * \fn OGRErr OGRGeometry::exportToWkt( char ** ppszDstText,
 * OGRwkbVariant variant = wkbVariantOldOgc ) const;
 *
 * \brief Convert a geometry into well known text format.
 *
 * This method relates to the SFCOM IWks::ExportToWKT() method.
 *
 * This method is the same as the C function OGR_G_ExportToWkt().
 *
 * @param ppszDstText a text buffer is allocated by the program, and assigned
 *                    to the passed pointer. After use, *ppszDstText should be
 *                    freed with CPLFree().
 * @param variant the specification that must be conformed too :
 *                    - wkbVariantOgc for old-style 99-402 extended
 *                      dimension (Z) WKB types
 *                    - wkbVariantIso for SFSQL 1.2 and ISO SQL/MM Part 3
 *
 * @return Currently OGRERR_NONE is always returned.
 */
OGRErr OGRGeometry::exportToWkt(char **ppszDstText, OGRwkbVariant variant) const
{
    OGRWktOptions opts;
    opts.variant = variant;
    OGRErr err(OGRERR_NONE);

    std::string wkt = exportToWkt(opts, &err);
    *ppszDstText = CPLStrdup(wkt.data());
    return err;
}

/************************************************************************/
/*                         OGR_G_ExportToWkt()                          */
/************************************************************************/

/**
 * \brief Convert a geometry into well known text format.
 *
 * This function relates to the SFCOM IWks::ExportToWKT() method.
 *
 * For backward compatibility purposes, it exports the Old-style 99-402
 * extended dimension (Z) WKB types for types Point, LineString, Polygon,
 * MultiPoint, MultiLineString, MultiPolygon and GeometryCollection.
 * For other geometry types, it is equivalent to OGR_G_ExportToIsoWkt().
 *
 * This function is the same as the CPP method OGRGeometry::exportToWkt().
 *
 * @param hGeom handle on the geometry to convert to a text format from.
 * @param ppszSrcText a text buffer is allocated by the program, and assigned
 *                    to the passed pointer. After use, *ppszDstText should be
 *                    freed with CPLFree().
 *
 * @return Currently OGRERR_NONE is always returned.
 */

OGRErr OGR_G_ExportToWkt(OGRGeometryH hGeom, char **ppszSrcText)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_ExportToWkt", OGRERR_FAILURE);

    return OGRGeometry::FromHandle(hGeom)->exportToWkt(ppszSrcText);
}

/************************************************************************/
/*                      OGR_G_ExportToIsoWkt()                          */
/************************************************************************/

/**
 * \brief Convert a geometry into SFSQL 1.2 / ISO SQL/MM Part 3 well
 * known text format.
 *
 * This function relates to the SFCOM IWks::ExportToWKT() method.
 * It exports the SFSQL 1.2 and ISO SQL/MM Part 3 extended dimension
 * (Z&M) WKB types.
 *
 * This function is the same as the CPP method
 * OGRGeometry::exportToWkt(wkbVariantIso).
 *
 * @param hGeom handle on the geometry to convert to a text format from.
 * @param ppszSrcText a text buffer is allocated by the program, and assigned
 *                    to the passed pointer. After use, *ppszDstText should be
 *                    freed with CPLFree().
 *
 * @return Currently OGRERR_NONE is always returned.
 *
 * @since GDAL 2.0
 */

OGRErr OGR_G_ExportToIsoWkt(OGRGeometryH hGeom, char **ppszSrcText)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_ExportToIsoWkt", OGRERR_FAILURE);

    return OGRGeometry::FromHandle(hGeom)->exportToWkt(ppszSrcText,
                                                       wkbVariantIso);
}

/**
 * \fn OGRwkbGeometryType OGRGeometry::getGeometryType() const;
 *
 * \brief Fetch geometry type.
 *
 * Note that the geometry type may include the 2.5D flag.  To get a 2D
 * flattened version of the geometry type apply the wkbFlatten() macro
 * to the return result.
 *
 * This method is the same as the C function OGR_G_GetGeometryType().
 *
 * @return the geometry type code.
 */

/************************************************************************/
/*                       OGR_G_GetGeometryType()                        */
/************************************************************************/
/**
 * \brief Fetch geometry type.
 *
 * Note that the geometry type may include the 2.5D flag.  To get a 2D
 * flattened version of the geometry type apply the wkbFlatten() macro
 * to the return result.
 *
 * This function is the same as the CPP method OGRGeometry::getGeometryType().
 *
 * @param hGeom handle on the geometry to get type from.
 * @return the geometry type code.
 */

OGRwkbGeometryType OGR_G_GetGeometryType(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_GetGeometryType", wkbUnknown);

    return OGRGeometry::FromHandle(hGeom)->getGeometryType();
}

/**
 * \fn const char * OGRGeometry::getGeometryName() const;
 *
 * \brief Fetch WKT name for geometry type.
 *
 * There is no SFCOM analog to this method.
 *
 * This method is the same as the C function OGR_G_GetGeometryName().
 *
 * @return name used for this geometry type in well known text format.  The
 * returned pointer is to a static internal string and should not be modified
 * or freed.
 */

/************************************************************************/
/*                       OGR_G_GetGeometryName()                        */
/************************************************************************/
/**
 * \brief Fetch WKT name for geometry type.
 *
 * There is no SFCOM analog to this function.
 *
 * This function is the same as the CPP method OGRGeometry::getGeometryName().
 *
 * @param hGeom handle on the geometry to get name from.
 * @return name used for this geometry type in well known text format.
 */

const char *OGR_G_GetGeometryName(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_GetGeometryName", "");

    return OGRGeometry::FromHandle(hGeom)->getGeometryName();
}

/**
 * \fn OGRGeometry *OGRGeometry::clone() const;
 *
 * \brief Make a copy of this object.
 *
 * This method relates to the SFCOM IGeometry::clone() method.
 *
 * This method is the same as the C function OGR_G_Clone().
 *
 * @return a new object instance with the same geometry, and spatial
 * reference system as the original.
 */

/************************************************************************/
/*                            OGR_G_Clone()                             */
/************************************************************************/
/**
 * \brief Make a copy of this object.
 *
 * This function relates to the SFCOM IGeometry::clone() method.
 *
 * This function is the same as the CPP method OGRGeometry::clone().
 *
 * @param hGeom handle on the geometry to clone from.
 * @return a handle on the copy of the geometry with the spatial
 * reference system as the original.
 */

OGRGeometryH OGR_G_Clone(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_Clone", nullptr);

    return OGRGeometry::ToHandle(OGRGeometry::FromHandle(hGeom)->clone());
}

/**
 * \fn OGRSpatialReference *OGRGeometry::getSpatialReference();
 *
 * \brief Returns spatial reference system for object.
 *
 * This method relates to the SFCOM IGeometry::get_SpatialReference() method.
 *
 * This method is the same as the C function OGR_G_GetSpatialReference().
 *
 * @return a reference to the spatial reference object.  The object may be
 * shared with many geometry objects, and should not be modified.
 */

/************************************************************************/
/*                     OGR_G_GetSpatialReference()                      */
/************************************************************************/
/**
 * \brief Returns spatial reference system for geometry.
 *
 * This function relates to the SFCOM IGeometry::get_SpatialReference() method.
 *
 * This function is the same as the CPP method
 * OGRGeometry::getSpatialReference().
 *
 * @param hGeom handle on the geometry to get spatial reference from.
 * @return a reference to the spatial reference geometry, which should not be
 * modified.
 */

OGRSpatialReferenceH OGR_G_GetSpatialReference(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_GetSpatialReference", nullptr);

    return OGRSpatialReference::ToHandle(const_cast<OGRSpatialReference *>(
        OGRGeometry::FromHandle(hGeom)->getSpatialReference()));
}

/**
 * \fn void OGRGeometry::empty();
 *
 * \brief Clear geometry information.
 * This restores the geometry to its initial
 * state after construction, and before assignment of actual geometry.
 *
 * This method relates to the SFCOM IGeometry::Empty() method.
 *
 * This method is the same as the C function OGR_G_Empty().
 */

/************************************************************************/
/*                            OGR_G_Empty()                             */
/************************************************************************/
/**
 * \brief Clear geometry information.
 * This restores the geometry to its initial
 * state after construction, and before assignment of actual geometry.
 *
 * This function relates to the SFCOM IGeometry::Empty() method.
 *
 * This function is the same as the CPP method OGRGeometry::empty().
 *
 * @param hGeom handle on the geometry to empty.
 */

void OGR_G_Empty(OGRGeometryH hGeom)

{
    VALIDATE_POINTER0(hGeom, "OGR_G_Empty");

    OGRGeometry::FromHandle(hGeom)->empty();
}

/**
 * \fn OGRBoolean OGRGeometry::IsEmpty() const;
 *
 * \brief Returns TRUE (non-zero) if the object has no points.
 *
 * Normally this
 * returns FALSE except between when an object is instantiated and points
 * have been assigned.
 *
 * This method relates to the SFCOM IGeometry::IsEmpty() method.
 *
 * @return TRUE if object is empty, otherwise FALSE.
 */

/************************************************************************/
/*                         OGR_G_IsEmpty()                              */
/************************************************************************/

/**
 * \brief Test if the geometry is empty.
 *
 * This method is the same as the CPP method OGRGeometry::IsEmpty().
 *
 * @param hGeom The Geometry to test.
 *
 * @return TRUE if the geometry has no points, otherwise FALSE.
 */

int OGR_G_IsEmpty(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_IsEmpty", TRUE);

    return OGRGeometry::FromHandle(hGeom)->IsEmpty();
}

/************************************************************************/
/*                              IsValid()                               */
/************************************************************************/

/**
 * \brief Test if the geometry is valid.
 *
 * This method is the same as the C function OGR_G_IsValid().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always return
 * FALSE.
 *
 *
 * @return TRUE if the geometry has no points, otherwise FALSE.
 */

OGRBoolean OGRGeometry::IsValid() const

{
    if (IsSFCGALCompatible())
    {
#ifndef HAVE_SFCGAL

#ifdef HAVE_GEOS
        if (wkbFlatten(getGeometryType()) == wkbTriangle)
        {
            // go on
        }
        else
#endif
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "SFCGAL support not enabled.");
            return FALSE;
        }
#else
        sfcgal_init();
        sfcgal_geometry_t *poThis = OGRGeometry::OGRexportToSFCGAL(this);
        if (poThis == nullptr)
        {
            CPLError(CE_Failure, CPLE_IllegalArg,
                     "SFCGAL geometry returned is NULL");
            return FALSE;
        }

        const int res = sfcgal_geometry_is_valid(poThis);
        sfcgal_geometry_delete(poThis);
        return res == 1;
#endif
    }

    {
#ifndef HAVE_GEOS
        CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
        return FALSE;

#else
        OGRBoolean bResult = FALSE;

        // Some invalid geometries, such as lines with one point, or
        // rings that do not close, cannot be converted to GEOS.
        // For validity checking we initialize the GEOS context with
        // the warning handler as the error handler to avoid emitting
        // CE_Failure when a geometry cannot be converted to GEOS.
        GEOSContextHandle_t hGEOSCtxt =
            initGEOS_r(OGRGEOSWarningHandler, OGRGEOSWarningHandler);

        GEOSGeom hThisGeosGeom = exportToGEOS(hGEOSCtxt);

        if (hThisGeosGeom != nullptr)
        {
            bResult = GEOSisValid_r(hGEOSCtxt, hThisGeosGeom);
#ifdef DEBUG_VERBOSE
            if (!bResult)
            {
                char *pszReason = GEOSisValidReason_r(hGEOSCtxt, hThisGeosGeom);
                CPLDebug("OGR", "%s", pszReason);
                GEOSFree_r(hGEOSCtxt, pszReason);
            }
#endif
            GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);
        }
        freeGEOSContext(hGEOSCtxt);

        return bResult;

#endif  // HAVE_GEOS
    }
}

/************************************************************************/
/*                           OGR_G_IsValid()                            */
/************************************************************************/

/**
 * \brief Test if the geometry is valid.
 *
 * This function is the same as the C++ method OGRGeometry::IsValid().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always return
 * FALSE.
 *
 * @param hGeom The Geometry to test.
 *
 * @return TRUE if the geometry has no points, otherwise FALSE.
 */

int OGR_G_IsValid(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_IsValid", FALSE);

    return OGRGeometry::FromHandle(hGeom)->IsValid();
}

/************************************************************************/
/*                              IsSimple()                               */
/************************************************************************/

/**
 * \brief Test if the geometry is simple.
 *
 * This method is the same as the C function OGR_G_IsSimple().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always return
 * FALSE.
 *
 *
 * @return TRUE if the geometry has no points, otherwise FALSE.
 */

OGRBoolean OGRGeometry::IsSimple() const

{
#ifndef HAVE_GEOS
    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return FALSE;

#else

    OGRBoolean bResult = FALSE;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hThisGeosGeom = exportToGEOS(hGEOSCtxt);

    if (hThisGeosGeom != nullptr)
    {
        bResult = GEOSisSimple_r(hGEOSCtxt, hThisGeosGeom);
        GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);
    }
    freeGEOSContext(hGEOSCtxt);

    return bResult;

#endif  // HAVE_GEOS
}

/**
 * \brief Returns TRUE if the geometry is simple.
 *
 * Returns TRUE if the geometry has no anomalous geometric points, such
 * as self intersection or self tangency. The description of each
 * instantiable geometric class will include the specific conditions that
 * cause an instance of that class to be classified as not simple.
 *
 * This function is the same as the C++ method OGRGeometry::IsSimple() method.
 *
 * If OGR is built without the GEOS library, this function will always return
 * FALSE.
 *
 * @param hGeom The Geometry to test.
 *
 * @return TRUE if object is simple, otherwise FALSE.
 */

int OGR_G_IsSimple(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_IsSimple", TRUE);

    return OGRGeometry::FromHandle(hGeom)->IsSimple();
}

/************************************************************************/
/*                              IsRing()                               */
/************************************************************************/

/**
 * \brief Test if the geometry is a ring
 *
 * This method is the same as the C function OGR_G_IsRing().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always return
 * FALSE.
 *
 *
 * @return TRUE if the coordinates of the geometry form a ring, by checking
 * length and closure (self-intersection is not checked), otherwise FALSE.
 */

OGRBoolean OGRGeometry::IsRing() const

{
#ifndef HAVE_GEOS
    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return FALSE;

#else

    OGRBoolean bResult = FALSE;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hThisGeosGeom = exportToGEOS(hGEOSCtxt);

    if (hThisGeosGeom != nullptr)
    {
        bResult = GEOSisRing_r(hGEOSCtxt, hThisGeosGeom);
        GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);
    }
    freeGEOSContext(hGEOSCtxt);

    return bResult;

#endif  // HAVE_GEOS
}

/************************************************************************/
/*                            OGR_G_IsRing()                            */
/************************************************************************/

/**
 * \brief Test if the geometry is a ring
 *
 * This function is the same as the C++ method OGRGeometry::IsRing().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always return
 * FALSE.
 *
 * @param hGeom The Geometry to test.
 *
 * @return TRUE if the coordinates of the geometry form a ring, by checking
 * length and closure (self-intersection is not checked), otherwise FALSE.
 */

int OGR_G_IsRing(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_IsRing", FALSE);

    return OGRGeometry::FromHandle(hGeom)->IsRing();
}

/************************************************************************/
/*                     OGRFromOGCGeomType()                             */
/************************************************************************/

/** Map OGC geometry format type to corresponding OGR constants.
 * @param pszGeomType POINT[ ][Z][M], LINESTRING[ ][Z][M], etc...
 * @return OGR constant.
 */
OGRwkbGeometryType OGRFromOGCGeomType(const char *pszGeomType)
{
    OGRwkbGeometryType eType = wkbUnknown;
    bool bConvertTo3D = false;
    bool bIsMeasured = false;
    if (*pszGeomType != '\0')
    {
        char ch = pszGeomType[strlen(pszGeomType) - 1];
        if (ch == 'm' || ch == 'M')
        {
            bIsMeasured = true;
            if (strlen(pszGeomType) > 1)
                ch = pszGeomType[strlen(pszGeomType) - 2];
        }
        if (ch == 'z' || ch == 'Z')
        {
            bConvertTo3D = true;
        }
    }

    if (STARTS_WITH_CI(pszGeomType, "POINT"))
        eType = wkbPoint;
    else if (STARTS_WITH_CI(pszGeomType, "LINESTRING"))
        eType = wkbLineString;
    else if (STARTS_WITH_CI(pszGeomType, "POLYGON"))
        eType = wkbPolygon;
    else if (STARTS_WITH_CI(pszGeomType, "MULTIPOINT"))
        eType = wkbMultiPoint;
    else if (STARTS_WITH_CI(pszGeomType, "MULTILINESTRING"))
        eType = wkbMultiLineString;
    else if (STARTS_WITH_CI(pszGeomType, "MULTIPOLYGON"))
        eType = wkbMultiPolygon;
    else if (STARTS_WITH_CI(pszGeomType, "GEOMETRYCOLLECTION"))
        eType = wkbGeometryCollection;
    else if (STARTS_WITH_CI(pszGeomType, "CIRCULARSTRING"))
        eType = wkbCircularString;
    else if (STARTS_WITH_CI(pszGeomType, "COMPOUNDCURVE"))
        eType = wkbCompoundCurve;
    else if (STARTS_WITH_CI(pszGeomType, "CURVEPOLYGON"))
        eType = wkbCurvePolygon;
    else if (STARTS_WITH_CI(pszGeomType, "MULTICURVE"))
        eType = wkbMultiCurve;
    else if (STARTS_WITH_CI(pszGeomType, "MULTISURFACE"))
        eType = wkbMultiSurface;
    else if (STARTS_WITH_CI(pszGeomType, "TRIANGLE"))
        eType = wkbTriangle;
    else if (STARTS_WITH_CI(pszGeomType, "POLYHEDRALSURFACE"))
        eType = wkbPolyhedralSurface;
    else if (STARTS_WITH_CI(pszGeomType, "TIN"))
        eType = wkbTIN;
    else if (STARTS_WITH_CI(pszGeomType, "CURVE"))
        eType = wkbCurve;
    else if (STARTS_WITH_CI(pszGeomType, "SURFACE"))
        eType = wkbSurface;
    else
        eType = wkbUnknown;

    if (bConvertTo3D)
        eType = wkbSetZ(eType);
    if (bIsMeasured)
        eType = wkbSetM(eType);

    return eType;
}

/************************************************************************/
/*                     OGRToOGCGeomType()                               */
/************************************************************************/

/** Map OGR geometry format constants to corresponding OGC geometry type.
 * @param eGeomType OGR geometry type
 * @param bCamelCase Whether the return should be like "MultiPoint"
 *        (bCamelCase=true) or "MULTIPOINT" (bCamelCase=false, default)
 * @param bAddZM Whether to include Z, M or ZM suffix for non-2D geometries.
 *               Default is false.
 * @param bSpaceBeforeZM Whether to include a space character before the Z/M/ZM
 *                       suffix. Default is false.
 * @return string with OGC geometry type (without dimensionality)
 */
const char *OGRToOGCGeomType(OGRwkbGeometryType eGeomType, bool bCamelCase,
                             bool bAddZM, bool bSpaceBeforeZM)
{
    const char *pszRet = "";
    switch (wkbFlatten(eGeomType))
    {
        case wkbUnknown:
            pszRet = "Geometry";
            break;
        case wkbPoint:
            pszRet = "Point";
            break;
        case wkbLineString:
            pszRet = "LineString";
            break;
        case wkbPolygon:
            pszRet = "Polygon";
            break;
        case wkbMultiPoint:
            pszRet = "MultiPoint";
            break;
        case wkbMultiLineString:
            pszRet = "MultiLineString";
            break;
        case wkbMultiPolygon:
            pszRet = "MultiPolygon";
            break;
        case wkbGeometryCollection:
            pszRet = "GeometryCollection";
            break;
        case wkbCircularString:
            pszRet = "CircularString";
            break;
        case wkbCompoundCurve:
            pszRet = "CompoundCurve";
            break;
        case wkbCurvePolygon:
            pszRet = "CurvePolygon";
            break;
        case wkbMultiCurve:
            pszRet = "MultiCurve";
            break;
        case wkbMultiSurface:
            pszRet = "MultiSurface";
            break;
        case wkbTriangle:
            pszRet = "Triangle";
            break;
        case wkbPolyhedralSurface:
            pszRet = "PolyhedralSurface";
            break;
        case wkbTIN:
            pszRet = "Tin";
            break;
        case wkbCurve:
            pszRet = "Curve";
            break;
        case wkbSurface:
            pszRet = "Surface";
            break;
        default:
            break;
    }
    if (bAddZM)
    {
        const bool bHasZ = CPL_TO_BOOL(OGR_GT_HasZ(eGeomType));
        const bool bHasM = CPL_TO_BOOL(OGR_GT_HasM(eGeomType));
        if (bHasZ || bHasM)
        {
            if (bSpaceBeforeZM)
                pszRet = CPLSPrintf("%s ", pszRet);
            if (bHasZ)
                pszRet = CPLSPrintf("%sZ", pszRet);
            if (bHasM)
                pszRet = CPLSPrintf("%sM", pszRet);
        }
    }
    if (!bCamelCase)
        pszRet = CPLSPrintf("%s", CPLString(pszRet).toupper().c_str());
    return pszRet;
}

/************************************************************************/
/*                       OGRGeometryTypeToName()                        */
/************************************************************************/

/**
 * \brief Fetch a human readable name corresponding to an OGRwkbGeometryType
 * value.  The returned value should not be modified, or freed by the
 * application.
 *
 * This function is C callable.
 *
 * @param eType the geometry type.
 *
 * @return internal human readable string, or NULL on failure.
 */

const char *OGRGeometryTypeToName(OGRwkbGeometryType eType)

{
    bool b3D = wkbHasZ(eType);
    bool bMeasured = wkbHasM(eType);

    switch (wkbFlatten(eType))
    {
        case wkbUnknown:
            if (b3D && bMeasured)
                return "3D Measured Unknown (any)";
            else if (b3D)
                return "3D Unknown (any)";
            else if (bMeasured)
                return "Measured Unknown (any)";
            else
                return "Unknown (any)";

        case wkbPoint:
            if (b3D && bMeasured)
                return "3D Measured Point";
            else if (b3D)
                return "3D Point";
            else if (bMeasured)
                return "Measured Point";
            else
                return "Point";

        case wkbLineString:
            if (b3D && bMeasured)
                return "3D Measured Line String";
            else if (b3D)
                return "3D Line String";
            else if (bMeasured)
                return "Measured Line String";
            else
                return "Line String";

        case wkbPolygon:
            if (b3D && bMeasured)
                return "3D Measured Polygon";
            else if (b3D)
                return "3D Polygon";
            else if (bMeasured)
                return "Measured Polygon";
            else
                return "Polygon";

        case wkbMultiPoint:
            if (b3D && bMeasured)
                return "3D Measured Multi Point";
            else if (b3D)
                return "3D Multi Point";
            else if (bMeasured)
                return "Measured Multi Point";
            else
                return "Multi Point";

        case wkbMultiLineString:
            if (b3D && bMeasured)
                return "3D Measured Multi Line String";
            else if (b3D)
                return "3D Multi Line String";
            else if (bMeasured)
                return "Measured Multi Line String";
            else
                return "Multi Line String";

        case wkbMultiPolygon:
            if (b3D && bMeasured)
                return "3D Measured Multi Polygon";
            else if (b3D)
                return "3D Multi Polygon";
            else if (bMeasured)
                return "Measured Multi Polygon";
            else
                return "Multi Polygon";

        case wkbGeometryCollection:
            if (b3D && bMeasured)
                return "3D Measured Geometry Collection";
            else if (b3D)
                return "3D Geometry Collection";
            else if (bMeasured)
                return "Measured Geometry Collection";
            else
                return "Geometry Collection";

        case wkbCircularString:
            if (b3D && bMeasured)
                return "3D Measured Circular String";
            else if (b3D)
                return "3D Circular String";
            else if (bMeasured)
                return "Measured Circular String";
            else
                return "Circular String";

        case wkbCompoundCurve:
            if (b3D && bMeasured)
                return "3D Measured Compound Curve";
            else if (b3D)
                return "3D Compound Curve";
            else if (bMeasured)
                return "Measured Compound Curve";
            else
                return "Compound Curve";

        case wkbCurvePolygon:
            if (b3D && bMeasured)
                return "3D Measured Curve Polygon";
            else if (b3D)
                return "3D Curve Polygon";
            else if (bMeasured)
                return "Measured Curve Polygon";
            else
                return "Curve Polygon";

        case wkbMultiCurve:
            if (b3D && bMeasured)
                return "3D Measured Multi Curve";
            else if (b3D)
                return "3D Multi Curve";
            else if (bMeasured)
                return "Measured Multi Curve";
            else
                return "Multi Curve";

        case wkbMultiSurface:
            if (b3D && bMeasured)
                return "3D Measured Multi Surface";
            else if (b3D)
                return "3D Multi Surface";
            else if (bMeasured)
                return "Measured Multi Surface";
            else
                return "Multi Surface";

        case wkbCurve:
            if (b3D && bMeasured)
                return "3D Measured Curve";
            else if (b3D)
                return "3D Curve";
            else if (bMeasured)
                return "Measured Curve";
            else
                return "Curve";

        case wkbSurface:
            if (b3D && bMeasured)
                return "3D Measured Surface";
            else if (b3D)
                return "3D Surface";
            else if (bMeasured)
                return "Measured Surface";
            else
                return "Surface";

        case wkbTriangle:
            if (b3D && bMeasured)
                return "3D Measured Triangle";
            else if (b3D)
                return "3D Triangle";
            else if (bMeasured)
                return "Measured Triangle";
            else
                return "Triangle";

        case wkbPolyhedralSurface:
            if (b3D && bMeasured)
                return "3D Measured PolyhedralSurface";
            else if (b3D)
                return "3D PolyhedralSurface";
            else if (bMeasured)
                return "Measured PolyhedralSurface";
            else
                return "PolyhedralSurface";

        case wkbTIN:
            if (b3D && bMeasured)
                return "3D Measured TIN";
            else if (b3D)
                return "3D TIN";
            else if (bMeasured)
                return "Measured TIN";
            else
                return "TIN";

        case wkbNone:
            return "None";

        default:
        {
            return CPLSPrintf("Unrecognized: %d", static_cast<int>(eType));
        }
    }
}

/************************************************************************/
/*                       OGRMergeGeometryTypes()                        */
/************************************************************************/

/**
 * \brief Find common geometry type.
 *
 * Given two geometry types, find the most specific common
 * type.  Normally used repeatedly with the geometries in a
 * layer to try and establish the most specific geometry type
 * that can be reported for the layer.
 *
 * NOTE: wkbUnknown is the "worst case" indicating a mixture of
 * geometry types with nothing in common but the base geometry
 * type.  wkbNone should be used to indicate that no geometries
 * have been encountered yet, and means the first geometry
 * encountered will establish the preliminary type.
 *
 * @param eMain the first input geometry type.
 * @param eExtra the second input geometry type.
 *
 * @return the merged geometry type.
 */

OGRwkbGeometryType OGRMergeGeometryTypes(OGRwkbGeometryType eMain,
                                         OGRwkbGeometryType eExtra)

{
    return OGRMergeGeometryTypesEx(eMain, eExtra, FALSE);
}

/**
 * \brief Find common geometry type.
 *
 * Given two geometry types, find the most specific common
 * type.  Normally used repeatedly with the geometries in a
 * layer to try and establish the most specific geometry type
 * that can be reported for the layer.
 *
 * NOTE: wkbUnknown is the "worst case" indicating a mixture of
 * geometry types with nothing in common but the base geometry
 * type.  wkbNone should be used to indicate that no geometries
 * have been encountered yet, and means the first geometry
 * encountered will establish the preliminary type.
 *
 * If bAllowPromotingToCurves is set to TRUE, mixing Polygon and CurvePolygon
 * will return CurvePolygon. Mixing LineString, CircularString, CompoundCurve
 * will return CompoundCurve. Mixing MultiPolygon and MultiSurface will return
 * MultiSurface. Mixing MultiCurve and MultiLineString will return MultiCurve.
 *
 * @param eMain the first input geometry type.
 * @param eExtra the second input geometry type.
 * @param bAllowPromotingToCurves determine if promotion to curve type
 * must be done.
 *
 * @return the merged geometry type.
 *
 * @since GDAL 2.0
 */

OGRwkbGeometryType OGRMergeGeometryTypesEx(OGRwkbGeometryType eMain,
                                           OGRwkbGeometryType eExtra,
                                           int bAllowPromotingToCurves)

{
    OGRwkbGeometryType eFMain = wkbFlatten(eMain);
    OGRwkbGeometryType eFExtra = wkbFlatten(eExtra);

    const bool bHasZ = (wkbHasZ(eMain) || wkbHasZ(eExtra));
    const bool bHasM = (wkbHasM(eMain) || wkbHasM(eExtra));

    if (eFMain == wkbUnknown || eFExtra == wkbUnknown)
        return OGR_GT_SetModifier(wkbUnknown, bHasZ, bHasM);

    if (eFMain == wkbNone)
        return eExtra;

    if (eFExtra == wkbNone)
        return eMain;

    if (eFMain == eFExtra)
    {
        return OGR_GT_SetModifier(eFMain, bHasZ, bHasM);
    }

    if (bAllowPromotingToCurves)
    {
        if (OGR_GT_IsCurve(eFMain) && OGR_GT_IsCurve(eFExtra))
            return OGR_GT_SetModifier(wkbCompoundCurve, bHasZ, bHasM);

        if (OGR_GT_IsSubClassOf(eFMain, eFExtra))
            return OGR_GT_SetModifier(eFExtra, bHasZ, bHasM);

        if (OGR_GT_IsSubClassOf(eFExtra, eFMain))
            return OGR_GT_SetModifier(eFMain, bHasZ, bHasM);
    }

    // One is subclass of the other one
    if (OGR_GT_IsSubClassOf(eFMain, eFExtra))
    {
        return OGR_GT_SetModifier(eFExtra, bHasZ, bHasM);
    }
    else if (OGR_GT_IsSubClassOf(eFExtra, eFMain))
    {
        return OGR_GT_SetModifier(eFMain, bHasZ, bHasM);
    }

    // Nothing apparently in common.
    return OGR_GT_SetModifier(wkbUnknown, bHasZ, bHasM);
}

/**
 * \fn void OGRGeometry::flattenTo2D();
 *
 * \brief Convert geometry to strictly 2D.
 * In a sense this converts all Z coordinates
 * to 0.0.
 *
 * This method is the same as the C function OGR_G_FlattenTo2D().
 */

/************************************************************************/
/*                         OGR_G_FlattenTo2D()                          */
/************************************************************************/
/**
 * \brief Convert geometry to strictly 2D.
 * In a sense this converts all Z coordinates
 * to 0.0.
 *
 * This function is the same as the CPP method OGRGeometry::flattenTo2D().
 *
 * @param hGeom handle on the geometry to convert.
 */

void OGR_G_FlattenTo2D(OGRGeometryH hGeom)

{
    OGRGeometry::FromHandle(hGeom)->flattenTo2D();
}

/************************************************************************/
/*                            exportToGML()                             */
/************************************************************************/

/**
 * \fn char *OGRGeometry::exportToGML( const char* const *
 * papszOptions = NULL ) const;
 *
 * \brief Convert a geometry into GML format.
 *
 * The GML geometry is expressed directly in terms of GML basic data
 * types assuming the this is available in the gml namespace.  The returned
 * string should be freed with CPLFree() when no longer required.
 *
 * The supported options are :
 * <ul>
 * <li> FORMAT=GML2/GML3/GML32 (GML2 or GML32 added in GDAL 2.1).
 *      If not set, it will default to GML 2.1.2 output.
 * </li>
 * <li> GML3_LINESTRING_ELEMENT=curve. (Only valid for FORMAT=GML3)
 *      To use gml:Curve element for linestrings.
 *      Otherwise gml:LineString will be used .
 * </li>
 * <li> GML3_LONGSRS=YES/NO. (Only valid for FORMAT=GML3, deprecated by
 *      SRSNAME_FORMAT in GDAL &gt;=2.2). Defaults to YES.
 *      If YES, SRS with EPSG authority will be written with the
 *      "urn:ogc:def:crs:EPSG::" prefix.
 *      In the case the SRS should be treated as lat/long or
 *      northing/easting, then the function will take care of coordinate order
 *      swapping if the data axis to CRS axis mapping indicates it.
 *      If set to NO, SRS with EPSG authority will be written with the "EPSG:"
 *      prefix, even if they are in lat/long order.
 * </li>
 * <li> SRSNAME_FORMAT=SHORT/OGC_URN/OGC_URL (Only valid for FORMAT=GML3, added
 *      in GDAL 2.2). Defaults to OGC_URN.  If SHORT, then srsName will be in
 *      the form AUTHORITY_NAME:AUTHORITY_CODE. If OGC_URN, then srsName will be
 *      in the form urn:ogc:def:crs:AUTHORITY_NAME::AUTHORITY_CODE. If OGC_URL,
 *      then srsName will be in the form
 *      http://www.opengis.net/def/crs/AUTHORITY_NAME/0/AUTHORITY_CODE. For
 *      OGC_URN and OGC_URL, in the case the SRS should be treated as lat/long
 *      or northing/easting, then the function will take care of coordinate
 *      order swapping if the data axis to CRS axis mapping indicates it.
 * </li>
 * <li> GMLID=astring. If specified, a gml:id attribute will be written in the
 *      top-level geometry element with the provided value.
 *      Required for GML 3.2 compatibility.
 * </li>
 * <li> SRSDIMENSION_LOC=POSLIST/GEOMETRY/GEOMETRY,POSLIST. (Only valid for
 *      FORMAT=GML3/GML32, GDAL >= 2.0) Default to POSLIST.
 *      For 2.5D geometries, define the location where to attach the
 *      srsDimension attribute.
 *      There are diverging implementations. Some put in on the
 *      &lt;gml:posList&gt; element, other on the top geometry element.
 * </li>
 * <li> NAMESPACE_DECL=YES/NO. If set to YES,
 *      xmlns:gml="http://www.opengis.net/gml" will be added to the root node
 *      for GML < 3.2 or xmlns:gml="http://www.opengis.net/gml/3.2" for GML 3.2
 * </li>
 * <li> XY_COORD_RESOLUTION=double (added in GDAL 3.9):
 *      Resolution for the coordinate precision of the X and Y coordinates.
 *      Expressed in the units of the X and Y axis of the SRS. eg 1e-5 for up
 *      to 5 decimal digits. 0 for the default behavior.
 * </li>
 * <li> Z_COORD_RESOLUTION=double (added in GDAL 3.9):
 *      Resolution for the coordinate precision of the Z coordinates.
 *      Expressed in the units of the Z axis of the SRS.
 *      0 for the default behavior.
 * </li>
 * </ul>
 *
 * This method is the same as the C function OGR_G_ExportToGMLEx().
 *
 * @param papszOptions NULL-terminated list of options.
 * @return A GML fragment to be freed with CPLFree() or NULL in case of error.
 */

char *OGRGeometry::exportToGML(const char *const *papszOptions) const
{
    return OGR_G_ExportToGMLEx(
        OGRGeometry::ToHandle(const_cast<OGRGeometry *>(this)),
        const_cast<char **>(papszOptions));
}

/************************************************************************/
/*                            exportToKML()                             */
/************************************************************************/

/**
 * \fn char *OGRGeometry::exportToKML() const;
 *
 * \brief Convert a geometry into KML format.
 *
 * The returned string should be freed with CPLFree() when no longer required.
 *
 * This method is the same as the C function OGR_G_ExportToKML().
 *
 * @return A KML fragment to be freed with CPLFree() or NULL in case of error.
 */

char *OGRGeometry::exportToKML() const
{
    return OGR_G_ExportToKML(
        OGRGeometry::ToHandle(const_cast<OGRGeometry *>(this)), nullptr);
}

/************************************************************************/
/*                            exportToJson()                             */
/************************************************************************/

/**
 * \fn char *OGRGeometry::exportToJson() const;
 *
 * \brief Convert a geometry into GeoJSON format.
 *
 * The returned string should be freed with CPLFree() when no longer required.
 *
 * The following options are supported :
 * <ul>
 * <li>XY_COORD_PRECISION=integer: number of decimal figures for X,Y coordinates
 * (added in GDAL 3.9)</li>
 * <li>Z_COORD_PRECISION=integer: number of decimal figures for Z coordinates
 * (added in GDAL 3.9)</li>
 * </ul>
 *
 * This method is the same as the C function OGR_G_ExportToJson().
 *
 * @param papszOptions Null terminated list of options, or null (added in 3.9)
 * @return A GeoJSON fragment to be freed with CPLFree() or NULL in case of error.
 */

char *OGRGeometry::exportToJson(CSLConstList papszOptions) const
{
    OGRGeometry *poGeometry = const_cast<OGRGeometry *>(this);
    return OGR_G_ExportToJsonEx(OGRGeometry::ToHandle(poGeometry),
                                const_cast<char **>(papszOptions));
}

/************************************************************************/
/*                 OGRSetGenerate_DB2_V72_BYTE_ORDER()                  */
/************************************************************************/

/**
 * \brief Special entry point to enable the hack for generating DB2 V7.2 style
 * WKB.
 *
 * DB2 seems to have placed (and require) an extra 0x30 or'ed with the byte
 * order in WKB.  This entry point is used to turn on or off the generation of
 * such WKB.
 */
OGRErr OGRSetGenerate_DB2_V72_BYTE_ORDER(int bGenerate_DB2_V72_BYTE_ORDER)

{
#if defined(HACK_FOR_IBM_DB2_V72)
    OGRGeometry::bGenerate_DB2_V72_BYTE_ORDER = bGenerate_DB2_V72_BYTE_ORDER;
    return OGRERR_NONE;
#else
    if (bGenerate_DB2_V72_BYTE_ORDER)
        return OGRERR_FAILURE;
    else
        return OGRERR_NONE;
#endif
}

/************************************************************************/
/*                 OGRGetGenerate_DB2_V72_BYTE_ORDER()                  */
/*                                                                      */
/*      This is a special entry point to get the value of static flag   */
/*      OGRGeometry::bGenerate_DB2_V72_BYTE_ORDER.                      */
/************************************************************************/
int OGRGetGenerate_DB2_V72_BYTE_ORDER()
{
    return OGRGeometry::bGenerate_DB2_V72_BYTE_ORDER;
}

/************************************************************************/
/*                          createGEOSContext()                         */
/************************************************************************/

/** Create a new GEOS context.
 * @return a new GEOS context (to be freed with freeGEOSContext())
 */
GEOSContextHandle_t OGRGeometry::createGEOSContext()
{
#ifndef HAVE_GEOS
    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;
#else
    return initGEOS_r(OGRGEOSWarningHandler, OGRGEOSErrorHandler);
#endif
}

/************************************************************************/
/*                          freeGEOSContext()                           */
/************************************************************************/

/** Destroy a GEOS context.
 * @param hGEOSCtxt GEOS context
 */
void OGRGeometry::freeGEOSContext(
    UNUSED_IF_NO_GEOS GEOSContextHandle_t hGEOSCtxt)
{
#ifdef HAVE_GEOS
    if (hGEOSCtxt != nullptr)
    {
        finishGEOS_r(hGEOSCtxt);
    }
#endif
}

#ifdef HAVE_GEOS

/************************************************************************/
/*                          convertToGEOSGeom()                         */
/************************************************************************/

static GEOSGeom convertToGEOSGeom(GEOSContextHandle_t hGEOSCtxt,
                                  OGRGeometry *poGeom)
{
    GEOSGeom hGeom = nullptr;
    const size_t nDataSize = poGeom->WkbSize();
    unsigned char *pabyData =
        static_cast<unsigned char *>(CPLMalloc(nDataSize));
#if GEOS_VERSION_MAJOR > 3 ||                                                  \
    (GEOS_VERSION_MAJOR == 3 && GEOS_VERSION_MINOR >= 12)
    OGRwkbVariant eWkbVariant = wkbVariantIso;
#else
    OGRwkbVariant eWkbVariant = wkbVariantOldOgc;
#endif
    if (poGeom->exportToWkb(wkbNDR, pabyData, eWkbVariant) == OGRERR_NONE)
        hGeom = GEOSGeomFromWKB_buf_r(hGEOSCtxt, pabyData, nDataSize);
    CPLFree(pabyData);
    return hGeom;
}
#endif

/************************************************************************/
/*                            exportToGEOS()                            */
/************************************************************************/

/** Returns a GEOSGeom object corresponding to the geometry.
 *
 * @param hGEOSCtxt GEOS context
 * @param bRemoveEmptyParts Whether empty parts of the geometry should be
 * removed before exporting to GEOS (GDAL >= 3.10)
 * @return a GEOSGeom object corresponding to the geometry (to be freed with
 * GEOSGeom_destroy_r()), or NULL in case of error
 */
GEOSGeom
OGRGeometry::exportToGEOS(UNUSED_IF_NO_GEOS GEOSContextHandle_t hGEOSCtxt,
                          UNUSED_IF_NO_GEOS bool bRemoveEmptyParts) const

{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;

#else

    if (hGEOSCtxt == nullptr)
        return nullptr;

    const OGRwkbGeometryType eType = wkbFlatten(getGeometryType());
#if (GEOS_VERSION_MAJOR == 3 && GEOS_VERSION_MINOR < 12)
    // POINT EMPTY is exported to WKB as if it were POINT(0 0),
    // so that particular case is necessary.
    if (eType == wkbPoint && IsEmpty())
    {
        return GEOSGeomFromWKT_r(hGEOSCtxt, "POINT EMPTY");
    }
#endif

    GEOSGeom hGeom = nullptr;

    OGRGeometry *poLinearGeom = nullptr;
    if (hasCurveGeometry())
    {
        poLinearGeom = getLinearGeometry();
        if (bRemoveEmptyParts)
            poLinearGeom->removeEmptyParts();
#if (GEOS_VERSION_MAJOR == 3 && GEOS_VERSION_MINOR < 12)
        // GEOS < 3.12 doesn't support M dimension
        if (poLinearGeom->IsMeasured())
            poLinearGeom->setMeasured(FALSE);
#endif
    }
    else
    {
        poLinearGeom = const_cast<OGRGeometry *>(this);
#if (GEOS_VERSION_MAJOR == 3 && GEOS_VERSION_MINOR < 12)
        // GEOS < 3.12 doesn't support M dimension
        if (IsMeasured())
        {
            poLinearGeom = clone();
            if (bRemoveEmptyParts)
                poLinearGeom->removeEmptyParts();
            poLinearGeom->setMeasured(FALSE);
        }
        else
#endif
            if (bRemoveEmptyParts && hasEmptyParts())
        {
            poLinearGeom = clone();
            poLinearGeom->removeEmptyParts();
        }
    }
    if (eType == wkbTriangle)
    {
        OGRPolygon oPolygon(*(poLinearGeom->toPolygon()));
        hGeom = convertToGEOSGeom(hGEOSCtxt, &oPolygon);
    }
    else if (eType == wkbPolyhedralSurface || eType == wkbTIN)
    {
        OGRGeometry *poGC = OGRGeometryFactory::forceTo(
            poLinearGeom->clone(),
            OGR_GT_SetModifier(wkbGeometryCollection, poLinearGeom->Is3D(),
                               poLinearGeom->IsMeasured()),
            nullptr);
        hGeom = convertToGEOSGeom(hGEOSCtxt, poGC);
        delete poGC;
    }
    else if (eType == wkbGeometryCollection)
    {
        bool bCanConvertToMultiPoly = true;
        // bool bMustConvertToMultiPoly = true;
        const OGRGeometryCollection *poGC =
            poLinearGeom->toGeometryCollection();
        for (int iGeom = 0; iGeom < poGC->getNumGeometries(); iGeom++)
        {
            const OGRwkbGeometryType eSubGeomType =
                wkbFlatten(poGC->getGeometryRef(iGeom)->getGeometryType());
            if (eSubGeomType == wkbPolyhedralSurface || eSubGeomType == wkbTIN)
            {
                // bMustConvertToMultiPoly = true;
            }
            else if (eSubGeomType != wkbMultiPolygon &&
                     eSubGeomType != wkbPolygon)
            {
                bCanConvertToMultiPoly = false;
                break;
            }
        }
        if (bCanConvertToMultiPoly /* && bMustConvertToMultiPoly */)
        {
            OGRGeometry *poMultiPolygon = OGRGeometryFactory::forceTo(
                poLinearGeom->clone(),
                OGR_GT_SetModifier(wkbMultiPolygon, poLinearGeom->Is3D(),
                                   poLinearGeom->IsMeasured()),
                nullptr);
            OGRGeometry *poGCDest = OGRGeometryFactory::forceTo(
                poMultiPolygon,
                OGR_GT_SetModifier(wkbGeometryCollection, poLinearGeom->Is3D(),
                                   poLinearGeom->IsMeasured()),
                nullptr);
            hGeom = convertToGEOSGeom(hGEOSCtxt, poGCDest);
            delete poGCDest;
        }
        else
        {
            hGeom = convertToGEOSGeom(hGEOSCtxt, poLinearGeom);
        }
    }
    else
    {
        hGeom = convertToGEOSGeom(hGEOSCtxt, poLinearGeom);
    }

    if (poLinearGeom != this)
        delete poLinearGeom;

    return hGeom;

#endif  // HAVE_GEOS
}

/************************************************************************/
/*                         hasCurveGeometry()                           */
/************************************************************************/

/**
 * \brief Returns if this geometry is or has curve geometry.
 *
 * Returns if a geometry is, contains or may contain a CIRCULARSTRING,
 * COMPOUNDCURVE, CURVEPOLYGON, MULTICURVE or MULTISURFACE.
 *
 * If bLookForNonLinear is set to TRUE, it will be actually looked if
 * the geometry or its subgeometries are or contain a non-linear
 * geometry in them. In which case, if the method returns TRUE, it
 * means that getLinearGeometry() would return an approximate version
 * of the geometry. Otherwise, getLinearGeometry() would do a
 * conversion, but with just converting container type, like
 * COMPOUNDCURVE -> LINESTRING, MULTICURVE -> MULTILINESTRING or
 * MULTISURFACE -> MULTIPOLYGON, resulting in a "loss-less"
 * conversion.
 *
 * This method is the same as the C function OGR_G_HasCurveGeometry().
 *
 * @param bLookForNonLinear set it to TRUE to check if the geometry is
 * or contains a CIRCULARSTRING.
 *
 * @return TRUE if this geometry is or has curve geometry.
 *
 * @since GDAL 2.0
 */

OGRBoolean OGRGeometry::hasCurveGeometry(CPL_UNUSED int bLookForNonLinear) const
{
    return FALSE;
}

/************************************************************************/
/*                         getLinearGeometry()                        */
/************************************************************************/

/**
 * \brief Return, possibly approximate, non-curve version of this geometry.
 *
 * Returns a geometry that has no CIRCULARSTRING, COMPOUNDCURVE, CURVEPOLYGON,
 * MULTICURVE or MULTISURFACE in it, by approximating curve geometries.
 *
 * The ownership of the returned geometry belongs to the caller.
 *
 * The reverse method is OGRGeometry::getCurveGeometry().
 *
 * This method is the same as the C function OGR_G_GetLinearGeometry().
 *
 * @param dfMaxAngleStepSizeDegrees the largest step in degrees along the
 * arc, zero to use the default setting.
 * @param papszOptions options as a null-terminated list of strings.
 *                     See OGRGeometryFactory::curveToLineString() for
 *                     valid options.
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 *
 * @since GDAL 2.0
 */

OGRGeometry *
OGRGeometry::getLinearGeometry(CPL_UNUSED double dfMaxAngleStepSizeDegrees,
                               CPL_UNUSED const char *const *papszOptions) const
{
    return clone();
}

/************************************************************************/
/*                             getCurveGeometry()                       */
/************************************************************************/

/**
 * \brief Return curve version of this geometry.
 *
 * Returns a geometry that has possibly CIRCULARSTRING, COMPOUNDCURVE,
 * CURVEPOLYGON, MULTICURVE or MULTISURFACE in it, by de-approximating
 * curve geometries.
 *
 * If the geometry has no curve portion, the returned geometry will be a clone
 * of it.
 *
 * The ownership of the returned geometry belongs to the caller.
 *
 * The reverse method is OGRGeometry::getLinearGeometry().
 *
 * This function is the same as C function OGR_G_GetCurveGeometry().
 *
 * @param papszOptions options as a null-terminated list of strings.
 *                     Unused for now. Must be set to NULL.
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 *
 * @since GDAL 2.0
 */

OGRGeometry *
OGRGeometry::getCurveGeometry(CPL_UNUSED const char *const *papszOptions) const
{
    return clone();
}

/************************************************************************/
/*                              Distance()                              */
/************************************************************************/

/**
 * \brief Compute distance between two geometries.
 *
 * Returns the shortest distance between the two geometries. The distance is
 * expressed into the same unit as the coordinates of the geometries.
 *
 * This method is the same as the C function OGR_G_Distance().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param poOtherGeom the other geometry to compare against.
 *
 * @return the distance between the geometries or -1 if an error occurs.
 */

double OGRGeometry::Distance(const OGRGeometry *poOtherGeom) const

{
    if (nullptr == poOtherGeom)
    {
        CPLDebug("OGR",
                 "OGRGeometry::Distance called with NULL geometry pointer");
        return -1.0;
    }

    if (IsSFCGALCompatible() || poOtherGeom->IsSFCGALCompatible())
    {
#ifndef HAVE_SFCGAL

        CPLError(CE_Failure, CPLE_NotSupported, "SFCGAL support not enabled.");
        return -1.0;

#else

        sfcgal_geometry_t *poThis = OGRGeometry::OGRexportToSFCGAL(this);
        if (poThis == nullptr)
            return -1.0;

        sfcgal_geometry_t *poOther =
            OGRGeometry::OGRexportToSFCGAL(poOtherGeom);
        if (poOther == nullptr)
        {
            sfcgal_geometry_delete(poThis);
            return -1.0;
        }

        const double dfDistance = sfcgal_geometry_distance(poThis, poOther);

        sfcgal_geometry_delete(poThis);
        sfcgal_geometry_delete(poOther);

        return dfDistance > 0.0 ? dfDistance : -1.0;

#endif
    }

    else
    {
#ifndef HAVE_GEOS

        CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
        return -1.0;

#else

        GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
        // GEOSGeom is a pointer
        GEOSGeom hOther = poOtherGeom->exportToGEOS(hGEOSCtxt);
        GEOSGeom hThis = exportToGEOS(hGEOSCtxt);

        int bIsErr = 0;
        double dfDistance = 0.0;

        if (hThis != nullptr && hOther != nullptr)
        {
            bIsErr = GEOSDistance_r(hGEOSCtxt, hThis, hOther, &dfDistance);
        }

        GEOSGeom_destroy_r(hGEOSCtxt, hThis);
        GEOSGeom_destroy_r(hGEOSCtxt, hOther);
        freeGEOSContext(hGEOSCtxt);

        if (bIsErr > 0)
        {
            return dfDistance;
        }

        /* Calculations error */
        return -1.0;

#endif /* HAVE_GEOS */
    }
}

/************************************************************************/
/*                           OGR_G_Distance()                           */
/************************************************************************/
/**
 * \brief Compute distance between two geometries.
 *
 * Returns the shortest distance between the two geometries. The distance is
 * expressed into the same unit as the coordinates of the geometries.
 *
 * This function is the same as the C++ method OGRGeometry::Distance().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hFirst the first geometry to compare against.
 * @param hOther the other geometry to compare against.
 *
 * @return the distance between the geometries or -1 if an error occurs.
 */

double OGR_G_Distance(OGRGeometryH hFirst, OGRGeometryH hOther)

{
    VALIDATE_POINTER1(hFirst, "OGR_G_Distance", 0.0);

    return OGRGeometry::FromHandle(hFirst)->Distance(
        OGRGeometry::FromHandle(hOther));
}

/************************************************************************/
/*                             Distance3D()                             */
/************************************************************************/

/**
 * \brief Returns the 3D distance between two geometries
 *
 * The distance is expressed into the same unit as the coordinates of the
 * geometries.
 *
 * This method is built on the SFCGAL library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the SFCGAL library, this method will always return
 * -1.0
 *
 * This function is the same as the C function OGR_G_Distance3D().
 *
 * @return distance between the two geometries
 * @since GDAL 2.2
 */

double OGRGeometry::Distance3D(
    UNUSED_IF_NO_SFCGAL const OGRGeometry *poOtherGeom) const
{
    if (poOtherGeom == nullptr)
    {
        CPLDebug("OGR",
                 "OGRTriangle::Distance3D called with NULL geometry pointer");
        return -1.0;
    }

    if (!(poOtherGeom->Is3D() && Is3D()))
    {
        CPLDebug("OGR", "OGRGeometry::Distance3D called with two dimensional "
                        "geometry(geometries)");
        return -1.0;
    }

#ifndef HAVE_SFCGAL

    CPLError(CE_Failure, CPLE_NotSupported, "SFCGAL support not enabled.");
    return -1.0;

#else

    sfcgal_init();
    sfcgal_geometry_t *poThis = OGRGeometry::OGRexportToSFCGAL(this);
    if (poThis == nullptr)
        return -1.0;

    sfcgal_geometry_t *poOther = OGRGeometry::OGRexportToSFCGAL(poOtherGeom);
    if (poOther == nullptr)
    {
        sfcgal_geometry_delete(poThis);
        return -1.0;
    }

    const double dfDistance = sfcgal_geometry_distance_3d(poThis, poOther);

    sfcgal_geometry_delete(poThis);
    sfcgal_geometry_delete(poOther);

    return dfDistance > 0 ? dfDistance : -1.0;

#endif
}

/************************************************************************/
/*                           OGR_G_Distance3D()                         */
/************************************************************************/
/**
 * \brief Returns the 3D distance between two geometries
 *
 * The distance is expressed into the same unit as the coordinates of the
 * geometries.
 *
 * This method is built on the SFCGAL library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the SFCGAL library, this method will always return
 * -1.0
 *
 * This function is the same as the C++ method OGRGeometry::Distance3D().
 *
 * @param hFirst the first geometry to compare against.
 * @param hOther the other geometry to compare against.
 * @return distance between the two geometries
 * @since GDAL 2.2
 *
 * @return the distance between the geometries or -1 if an error occurs.
 */

double OGR_G_Distance3D(OGRGeometryH hFirst, OGRGeometryH hOther)

{
    VALIDATE_POINTER1(hFirst, "OGR_G_Distance3D", 0.0);

    return OGRGeometry::FromHandle(hFirst)->Distance3D(
        OGRGeometry::FromHandle(hOther));
}

/************************************************************************/
/*                       OGRGeometryRebuildCurves()                     */
/************************************************************************/

#ifdef HAVE_GEOS
static OGRGeometry *OGRGeometryRebuildCurves(const OGRGeometry *poGeom,
                                             const OGRGeometry *poOtherGeom,
                                             OGRGeometry *poOGRProduct)
{
    if (poOGRProduct != nullptr &&
        wkbFlatten(poOGRProduct->getGeometryType()) != wkbPoint &&
        (poGeom->hasCurveGeometry(true) ||
         (poOtherGeom && poOtherGeom->hasCurveGeometry(true))))
    {
        OGRGeometry *poCurveGeom = poOGRProduct->getCurveGeometry();
        delete poOGRProduct;
        return poCurveGeom;
    }
    return poOGRProduct;
}

/************************************************************************/
/*                       BuildGeometryFromGEOS()                        */
/************************************************************************/

static OGRGeometry *BuildGeometryFromGEOS(GEOSContextHandle_t hGEOSCtxt,
                                          GEOSGeom hGeosProduct,
                                          const OGRGeometry *poSelf,
                                          const OGRGeometry *poOtherGeom)
{
    OGRGeometry *poOGRProduct = nullptr;
    if (hGeosProduct != nullptr)
    {
        poOGRProduct =
            OGRGeometryFactory::createFromGEOS(hGEOSCtxt, hGeosProduct);
        if (poOGRProduct != nullptr &&
            poSelf->getSpatialReference() != nullptr &&
            (poOtherGeom == nullptr ||
             (poOtherGeom->getSpatialReference() != nullptr &&
              poOtherGeom->getSpatialReference()->IsSame(
                  poSelf->getSpatialReference()))))
        {
            poOGRProduct->assignSpatialReference(poSelf->getSpatialReference());
        }
        poOGRProduct =
            OGRGeometryRebuildCurves(poSelf, poOtherGeom, poOGRProduct);
        GEOSGeom_destroy_r(hGEOSCtxt, hGeosProduct);
    }
    return poOGRProduct;
}

/************************************************************************/
/*                     BuildGeometryFromTwoGeoms()                      */
/************************************************************************/

static OGRGeometry *BuildGeometryFromTwoGeoms(
    const OGRGeometry *poSelf, const OGRGeometry *poOtherGeom,
    GEOSGeometry *(*pfnGEOSFunction_r)(GEOSContextHandle_t,
                                       const GEOSGeometry *,
                                       const GEOSGeometry *))
{
    OGRGeometry *poOGRProduct = nullptr;

    GEOSContextHandle_t hGEOSCtxt = poSelf->createGEOSContext();
    GEOSGeom hThisGeosGeom = poSelf->exportToGEOS(hGEOSCtxt);
    GEOSGeom hOtherGeosGeom = poOtherGeom->exportToGEOS(hGEOSCtxt);
    if (hThisGeosGeom != nullptr && hOtherGeosGeom != nullptr)
    {
        GEOSGeom hGeosProduct =
            pfnGEOSFunction_r(hGEOSCtxt, hThisGeosGeom, hOtherGeosGeom);

        poOGRProduct =
            BuildGeometryFromGEOS(hGEOSCtxt, hGeosProduct, poSelf, poOtherGeom);
    }
    GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);
    GEOSGeom_destroy_r(hGEOSCtxt, hOtherGeosGeom);
    poSelf->freeGEOSContext(hGEOSCtxt);

    return poOGRProduct;
}

/************************************************************************/
/*                       OGRGEOSBooleanPredicate()                      */
/************************************************************************/

static OGRBoolean OGRGEOSBooleanPredicate(
    const OGRGeometry *poSelf, const OGRGeometry *poOtherGeom,
    char (*pfnGEOSFunction_r)(GEOSContextHandle_t, const GEOSGeometry *,
                              const GEOSGeometry *))
{
    OGRBoolean bResult = FALSE;

    GEOSContextHandle_t hGEOSCtxt = poSelf->createGEOSContext();
    GEOSGeom hThisGeosGeom = poSelf->exportToGEOS(hGEOSCtxt);
    GEOSGeom hOtherGeosGeom = poOtherGeom->exportToGEOS(hGEOSCtxt);
    if (hThisGeosGeom != nullptr && hOtherGeosGeom != nullptr)
    {
        bResult = pfnGEOSFunction_r(hGEOSCtxt, hThisGeosGeom, hOtherGeosGeom);
    }
    GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);
    GEOSGeom_destroy_r(hGEOSCtxt, hOtherGeosGeom);
    poSelf->freeGEOSContext(hGEOSCtxt);

    return bResult;
}

#endif  // HAVE_GEOS

/************************************************************************/
/*                            MakeValid()                               */
/************************************************************************/

/**
 * \brief Attempts to make an invalid geometry valid without losing vertices.
 *
 * Already-valid geometries are cloned without further intervention
 * for default MODE=LINEWORK. Already-valid geometries with MODE=STRUCTURE
 * may be subject to non-significant transformations, such as duplicated point
 * removal, change in ring winding order, etc. (before GDAL 3.10, single-part
 * geometry collections could be returned a single geometry. GDAL 3.10
 * returns the same type of geometry).
 *
 * Running OGRGeometryFactory::removeLowerDimensionSubGeoms() as a
 * post-processing step is often desired.
 *
 * This method is the same as the C function OGR_G_MakeValid().
 *
 * This function is built on the GEOS >= 3.8 library, check it for the
 * definition of the geometry operation. If OGR is built without the GEOS >= 3.8
 * library, this function will return a clone of the input geometry if it is
 * valid, or NULL if it is invalid
 *
 * @param papszOptions NULL terminated list of options, or NULL. The following
 * options are available:
 * <ul>
 * <li>METHOD=LINEWORK/STRUCTURE.
 *     LINEWORK is the default method, which combines all rings into a set of
 *     noded lines and then extracts valid polygons from that linework.
 *     The STRUCTURE method (requires GEOS >= 3.10 and GDAL >= 3.4) first makes
 *     all rings valid, then merges shells and
 *     subtracts holes from shells to generate valid result. Assumes that
 *     holes and shells are correctly categorized.</li>
 * <li>KEEP_COLLAPSED=YES/NO. Only for METHOD=STRUCTURE.
 *     NO (default): collapses are converted to empty geometries
 *     YES: collapses are converted to a valid geometry of lower dimension.</li>
 * </ul>
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 *
 * @since GDAL 3.0
 */
OGRGeometry *OGRGeometry::MakeValid(CSLConstList papszOptions) const
{
    (void)papszOptions;
#ifndef HAVE_GEOS
    if (IsValid())
        return clone();

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;
#else
    if (IsSFCGALCompatible())
    {
        if (IsValid())
            return clone();
    }
    else if (wkbFlatten(getGeometryType()) == wkbCurvePolygon)
    {
        GEOSContextHandle_t hGEOSCtxt = initGEOS_r(nullptr, nullptr);
        OGRBoolean bIsValid = FALSE;
        GEOSGeom hGeosGeom = exportToGEOS(hGEOSCtxt);
        if (hGeosGeom)
        {
            bIsValid = GEOSisValid_r(hGEOSCtxt, hGeosGeom);
            GEOSGeom_destroy_r(hGEOSCtxt, hGeosGeom);
        }
        freeGEOSContext(hGEOSCtxt);
        if (bIsValid)
            return clone();
    }

    const bool bStructureMethod = EQUAL(
        CSLFetchNameValueDef(papszOptions, "METHOD", "LINEWORK"), "STRUCTURE");
    CPL_IGNORE_RET_VAL(bStructureMethod);
#if !(GEOS_VERSION_MAJOR > 3 ||                                                \
      (GEOS_VERSION_MAJOR == 3 && GEOS_VERSION_MINOR >= 10))
    if (bStructureMethod)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "GEOS 3.10 or later needed for METHOD=STRUCTURE.");
        return nullptr;
    }
#endif

    OGRGeometry *poOGRProduct = nullptr;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hGeosGeom = exportToGEOS(hGEOSCtxt);
    if (hGeosGeom != nullptr)
    {
        GEOSGeom hGEOSRet;
#if GEOS_VERSION_MAJOR > 3 ||                                                  \
    (GEOS_VERSION_MAJOR == 3 && GEOS_VERSION_MINOR >= 10)
        if (bStructureMethod)
        {
            GEOSMakeValidParams *params =
                GEOSMakeValidParams_create_r(hGEOSCtxt);
            CPLAssert(params);
            GEOSMakeValidParams_setMethod_r(hGEOSCtxt, params,
                                            GEOS_MAKE_VALID_STRUCTURE);
            GEOSMakeValidParams_setKeepCollapsed_r(
                hGEOSCtxt, params,
                CPLFetchBool(papszOptions, "KEEP_COLLAPSED", false));
            hGEOSRet = GEOSMakeValidWithParams_r(hGEOSCtxt, hGeosGeom, params);
            GEOSMakeValidParams_destroy_r(hGEOSCtxt, params);
        }
        else
#endif
        {
            hGEOSRet = GEOSMakeValid_r(hGEOSCtxt, hGeosGeom);
        }
        GEOSGeom_destroy_r(hGEOSCtxt, hGeosGeom);

        if (hGEOSRet != nullptr)
        {
            poOGRProduct =
                OGRGeometryFactory::createFromGEOS(hGEOSCtxt, hGEOSRet);
            if (poOGRProduct != nullptr && getSpatialReference() != nullptr)
                poOGRProduct->assignSpatialReference(getSpatialReference());
            poOGRProduct =
                OGRGeometryRebuildCurves(this, nullptr, poOGRProduct);
            GEOSGeom_destroy_r(hGEOSCtxt, hGEOSRet);

#if GEOS_VERSION_MAJOR > 3 ||                                                  \
    (GEOS_VERSION_MAJOR == 3 && GEOS_VERSION_MINOR >= 10)
            // METHOD=STRUCTURE is not guaranteed to return a multiple geometry
            // if the input is a multiple geometry
            if (poOGRProduct && bStructureMethod &&
                OGR_GT_IsSubClassOf(getGeometryType(), wkbGeometryCollection) &&
                !OGR_GT_IsSubClassOf(poOGRProduct->getGeometryType(),
                                     wkbGeometryCollection))
            {
                poOGRProduct = OGRGeometryFactory::forceTo(poOGRProduct,
                                                           getGeometryType());
            }
#endif
        }
    }
    freeGEOSContext(hGEOSCtxt);

    return poOGRProduct;
#endif
}

/************************************************************************/
/*                         OGR_G_MakeValid()                            */
/************************************************************************/

/**
 * \brief Attempts to make an invalid geometry valid without losing vertices.
 *
 * Already-valid geometries are cloned without further intervention.
 *
 * This function is the same as the C++ method OGRGeometry::MakeValid().
 *
 * This function is built on the GEOS >= 3.8 library, check it for the
 * definition of the geometry operation. If OGR is built without the GEOS >= 3.8
 * library, this function will return a clone of the input geometry if it is
 * valid, or NULL if it is invalid
 *
 * @param hGeom The Geometry to make valid.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 *
 * @since GDAL 3.0
 */

OGRGeometryH OGR_G_MakeValid(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_MakeValid", nullptr);

    return OGRGeometry::ToHandle(OGRGeometry::FromHandle(hGeom)->MakeValid());
}

/************************************************************************/
/*                         OGR_G_MakeValidEx()                            */
/************************************************************************/

/**
 * \brief Attempts to make an invalid geometry valid without losing vertices.
 *
 * Already-valid geometries are cloned without further intervention.
 *
 * This function is the same as the C++ method OGRGeometry::MakeValid().
 *
 * See documentation of that method for possible options.
 *
 * @param hGeom The Geometry to make valid.
 * @param papszOptions Options.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 *
 * @since GDAL 3.4
 */

OGRGeometryH OGR_G_MakeValidEx(OGRGeometryH hGeom, CSLConstList papszOptions)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_MakeValidEx", nullptr);

    return OGRGeometry::ToHandle(
        OGRGeometry::FromHandle(hGeom)->MakeValid(papszOptions));
}

/************************************************************************/
/*                            Normalize()                               */
/************************************************************************/

/**
 * \brief Attempts to bring geometry into normalized/canonical form.
 *
 * This method is the same as the C function OGR_G_Normalize().
 *
 * This function is built on the GEOS library; check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 *
 * @since GDAL 3.3
 */
OGRGeometry *OGRGeometry::Normalize() const
{
#ifndef HAVE_GEOS
    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;
#else
    OGRGeometry *poOGRProduct = nullptr;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hGeosGeom = exportToGEOS(hGEOSCtxt);
    if (hGeosGeom != nullptr)
    {

        int hGEOSRet = GEOSNormalize_r(hGEOSCtxt, hGeosGeom);

        if (hGEOSRet == 0)
        {
            poOGRProduct =
                BuildGeometryFromGEOS(hGEOSCtxt, hGeosGeom, this, nullptr);
        }
        else
        {
            GEOSGeom_destroy_r(hGEOSCtxt, hGeosGeom);
        }
    }
    freeGEOSContext(hGEOSCtxt);

    return poOGRProduct;
#endif
}

/************************************************************************/
/*                         OGR_G_Normalize()                            */
/************************************************************************/

/**
 * \brief Attempts to bring geometry into normalized/canonical form.
 *
 * This function is the same as the C++ method OGRGeometry::Normalize().
 *
 * This function is built on the GEOS library; check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 * @param hGeom The Geometry to normalize.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 *
 * @since GDAL 3.3
 */

OGRGeometryH OGR_G_Normalize(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_Normalize", nullptr);

    return OGRGeometry::ToHandle(OGRGeometry::FromHandle(hGeom)->Normalize());
}

/************************************************************************/
/*                             ConvexHull()                             */
/************************************************************************/

/**
 * \brief Compute convex hull.
 *
 * A new geometry object is created and returned containing the convex
 * hull of the geometry on which the method is invoked.
 *
 * This method is the same as the C function OGR_G_ConvexHull().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 */

OGRGeometry *OGRGeometry::ConvexHull() const

{
    if (IsSFCGALCompatible())
    {
#ifndef HAVE_SFCGAL

        CPLError(CE_Failure, CPLE_NotSupported, "SFCGAL support not enabled.");
        return nullptr;

#else

        sfcgal_geometry_t *poThis = OGRGeometry::OGRexportToSFCGAL(this);
        if (poThis == nullptr)
            return nullptr;

        sfcgal_geometry_t *poRes = sfcgal_geometry_convexhull_3d(poThis);
        OGRGeometry *h_prodGeom = SFCGALexportToOGR(poRes);
        if (h_prodGeom)
            h_prodGeom->assignSpatialReference(getSpatialReference());

        sfcgal_geometry_delete(poThis);
        sfcgal_geometry_delete(poRes);

        return h_prodGeom;

#endif
    }

    else
    {
#ifndef HAVE_GEOS

        CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
        return nullptr;

#else

        OGRGeometry *poOGRProduct = nullptr;

        GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
        GEOSGeom hGeosGeom = exportToGEOS(hGEOSCtxt);
        if (hGeosGeom != nullptr)
        {
            GEOSGeom hGeosHull = GEOSConvexHull_r(hGEOSCtxt, hGeosGeom);
            GEOSGeom_destroy_r(hGEOSCtxt, hGeosGeom);

            poOGRProduct =
                BuildGeometryFromGEOS(hGEOSCtxt, hGeosHull, this, nullptr);
        }
        freeGEOSContext(hGEOSCtxt);

        return poOGRProduct;

#endif /* HAVE_GEOS */
    }
}

/************************************************************************/
/*                          OGR_G_ConvexHull()                          */
/************************************************************************/
/**
 * \brief Compute convex hull.
 *
 * A new geometry object is created and returned containing the convex
 * hull of the geometry on which the method is invoked.
 *
 * This function is the same as the C++ method OGRGeometry::ConvexHull().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hTarget The Geometry to calculate the convex hull of.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 */

OGRGeometryH OGR_G_ConvexHull(OGRGeometryH hTarget)

{
    VALIDATE_POINTER1(hTarget, "OGR_G_ConvexHull", nullptr);

    return OGRGeometry::ToHandle(
        OGRGeometry::FromHandle(hTarget)->ConvexHull());
}

/************************************************************************/
/*                             ConcaveHull()                            */
/************************************************************************/

/**
 * \brief Compute "concave hull" of a geometry.
 *
 * The concave hull is fully contained within the convex hull and also
 * contains all the points of the input, but in a smaller area.
 * The area ratio is the ratio of the area of the convex hull and the concave
 * hull. Frequently used to convert a multi-point into a polygonal area.
 * that contains all the points in the input Geometry.
 *
 * A new geometry object is created and returned containing the concave
 * hull of the geometry on which the method is invoked.
 *
 * This method is the same as the C function OGR_G_ConcaveHull().
 *
 * This method is built on the GEOS >= 3.11 library
 * If OGR is built without the GEOS >= 3.11 librray, this method will always
 * fail, issuing a CPLE_NotSupported error.
 *
 * @param dfRatio Ratio of the area of the convex hull and the concave hull.
 * @param bAllowHoles Whether holes are allowed.
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 *
 * @since GDAL 3.6
 */

OGRGeometry *OGRGeometry::ConcaveHull(double dfRatio, bool bAllowHoles) const
{
#ifndef HAVE_GEOS
    (void)dfRatio;
    (void)bAllowHoles;
    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;
#elif GEOS_VERSION_MAJOR == 3 && GEOS_VERSION_MINOR < 11
    (void)dfRatio;
    (void)bAllowHoles;
    CPLError(CE_Failure, CPLE_NotSupported,
             "GEOS 3.11 or later needed for ConcaveHull.");
    return nullptr;
#else
    OGRGeometry *poOGRProduct = nullptr;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hGeosGeom = exportToGEOS(hGEOSCtxt);
    if (hGeosGeom != nullptr)
    {
        GEOSGeom hGeosHull =
            GEOSConcaveHull_r(hGEOSCtxt, hGeosGeom, dfRatio, bAllowHoles);
        GEOSGeom_destroy_r(hGEOSCtxt, hGeosGeom);

        poOGRProduct =
            BuildGeometryFromGEOS(hGEOSCtxt, hGeosHull, this, nullptr);
    }
    freeGEOSContext(hGEOSCtxt);

    return poOGRProduct;
#endif /* HAVE_GEOS */
}

/************************************************************************/
/*                          OGR_G_ConcaveHull()                         */
/************************************************************************/
/**
 * \brief Compute "concave hull" of a geometry.
 *
 * The concave hull is fully contained within the convex hull and also
 * contains all the points of the input, but in a smaller area.
 * The area ratio is the ratio of the area of the convex hull and the concave
 * hull. Frequently used to convert a multi-point into a polygonal area.
 * that contains all the points in the input Geometry.
 *
 * A new geometry object is created and returned containing the convex
 * hull of the geometry on which the function is invoked.
 *
 * This function is the same as the C++ method OGRGeometry::ConcaveHull().
 *
 * This function is built on the GEOS >= 3.11 library
 * If OGR is built without the GEOS >= 3.11 librray, this function will always
 * fail, issuing a CPLE_NotSupported error.
 *
 * @param hTarget The Geometry to calculate the concave hull of.
 * @param dfRatio Ratio of the area of the convex hull and the concave hull.
 * @param bAllowHoles Whether holes are allowed.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 *
 * @since GDAL 3.6
 */

OGRGeometryH OGR_G_ConcaveHull(OGRGeometryH hTarget, double dfRatio,
                               bool bAllowHoles)

{
    VALIDATE_POINTER1(hTarget, "OGR_G_ConcaveHull", nullptr);

    return OGRGeometry::ToHandle(
        OGRGeometry::FromHandle(hTarget)->ConcaveHull(dfRatio, bAllowHoles));
}

/************************************************************************/
/*                            Boundary()                                */
/************************************************************************/

/**
 * \brief Compute boundary.
 *
 * A new geometry object is created and returned containing the boundary
 * of the geometry on which the method is invoked.
 *
 * This method is the same as the C function OGR_G_Boundary().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 *
 * @since OGR 1.8.0
 */

OGRGeometry *OGRGeometry::Boundary() const

{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;

#else

    OGRGeometry *poOGRProduct = nullptr;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hGeosGeom = exportToGEOS(hGEOSCtxt);
    if (hGeosGeom != nullptr)
    {
        GEOSGeom hGeosProduct = GEOSBoundary_r(hGEOSCtxt, hGeosGeom);
        GEOSGeom_destroy_r(hGEOSCtxt, hGeosGeom);

        poOGRProduct =
            BuildGeometryFromGEOS(hGEOSCtxt, hGeosProduct, this, nullptr);
    }
    freeGEOSContext(hGEOSCtxt);

    return poOGRProduct;

#endif  // HAVE_GEOS
}

//! @cond Doxygen_Suppress
/**
 * \brief Compute boundary (deprecated)
 *
 * @deprecated
 *
 * @see Boundary()
 */
OGRGeometry *OGRGeometry::getBoundary() const

{
    return Boundary();
}

//! @endcond

/************************************************************************/
/*                         OGR_G_Boundary()                             */
/************************************************************************/
/**
 * \brief Compute boundary.
 *
 * A new geometry object is created and returned containing the boundary
 * of the geometry on which the method is invoked.
 *
 * This function is the same as the C++ method OGR_G_Boundary().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hTarget The Geometry to calculate the boundary of.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 *
 * @since OGR 1.8.0
 */
OGRGeometryH OGR_G_Boundary(OGRGeometryH hTarget)

{
    VALIDATE_POINTER1(hTarget, "OGR_G_Boundary", nullptr);

    return OGRGeometry::ToHandle(OGRGeometry::FromHandle(hTarget)->Boundary());
}

/**
 * \brief Compute boundary (deprecated)
 *
 * @deprecated
 *
 * @see OGR_G_Boundary()
 */
OGRGeometryH OGR_G_GetBoundary(OGRGeometryH hTarget)

{
    VALIDATE_POINTER1(hTarget, "OGR_G_GetBoundary", nullptr);

    return OGRGeometry::ToHandle(OGRGeometry::FromHandle(hTarget)->Boundary());
}

/************************************************************************/
/*                               Buffer()                               */
/************************************************************************/

/**
 * \brief Compute buffer of geometry.
 *
 * Builds a new geometry containing the buffer region around the geometry
 * on which it is invoked.  The buffer is a polygon containing the region within
 * the buffer distance of the original geometry.
 *
 * Some buffer sections are properly described as curves, but are converted to
 * approximate polygons.  The nQuadSegs parameter can be used to control how
 * many segments should be used to define a 90 degree curve - a quadrant of a
 * circle.  A value of 30 is a reasonable default.  Large values result in
 * large numbers of vertices in the resulting buffer geometry while small
 * numbers reduce the accuracy of the result.
 *
 * This method is the same as the C function OGR_G_Buffer().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param dfDist the buffer distance to be applied. Should be expressed into
 *               the same unit as the coordinates of the geometry.
 *
 * @param nQuadSegs the number of segments used to approximate a 90
 * degree (quadrant) of curvature.
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 */

OGRGeometry *OGRGeometry::Buffer(UNUSED_IF_NO_GEOS double dfDist,
                                 UNUSED_IF_NO_GEOS int nQuadSegs) const

{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;

#else

    OGRGeometry *poOGRProduct = nullptr;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hGeosGeom = exportToGEOS(hGEOSCtxt);
    if (hGeosGeom != nullptr)
    {
        GEOSGeom hGeosProduct =
            GEOSBuffer_r(hGEOSCtxt, hGeosGeom, dfDist, nQuadSegs);
        GEOSGeom_destroy_r(hGEOSCtxt, hGeosGeom);

        poOGRProduct =
            BuildGeometryFromGEOS(hGEOSCtxt, hGeosProduct, this, nullptr);
    }
    freeGEOSContext(hGEOSCtxt);

    return poOGRProduct;

#endif  // HAVE_GEOS
}

/************************************************************************/
/*                            OGR_G_Buffer()                            */
/************************************************************************/

/**
 * \brief Compute buffer of geometry.
 *
 * Builds a new geometry containing the buffer region around the geometry
 * on which it is invoked.  The buffer is a polygon containing the region within
 * the buffer distance of the original geometry.
 *
 * Some buffer sections are properly described as curves, but are converted to
 * approximate polygons.  The nQuadSegs parameter can be used to control how
 * many segments should be used to define a 90 degree curve - a quadrant of a
 * circle.  A value of 30 is a reasonable default.  Large values result in
 * large numbers of vertices in the resulting buffer geometry while small
 * numbers reduce the accuracy of the result.
 *
 * This function is the same as the C++ method OGRGeometry::Buffer().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hTarget the geometry.
 * @param dfDist the buffer distance to be applied. Should be expressed into
 *               the same unit as the coordinates of the geometry.
 *
 * @param nQuadSegs the number of segments used to approximate a 90 degree
 * (quadrant) of curvature.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 */

OGRGeometryH OGR_G_Buffer(OGRGeometryH hTarget, double dfDist, int nQuadSegs)

{
    VALIDATE_POINTER1(hTarget, "OGR_G_Buffer", nullptr);

    return OGRGeometry::ToHandle(
        OGRGeometry::FromHandle(hTarget)->Buffer(dfDist, nQuadSegs));
}

/**
 * \brief Compute buffer of geometry.
 *
 * Builds a new geometry containing the buffer region around the geometry
 * on which it is invoked.  The buffer is a polygon containing the region within
 * the buffer distance of the original geometry.
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * The following options are supported. See the GEOS library for more detailed
 * descriptions.
 *
 * <ul>
 * <li>ENDCAP_STYLE=ROUND/FLAT/SQUARE</li>
 * <li>JOIN_STYLE=ROUND/MITRE/BEVEL</li>
 * <li>MITRE_LIMIT=double</li>
 * <li>QUADRANT_SEGMENTS=int</li>
 * <li>SINGLE_SIDED=YES/NO</li>
 * </ul>
 *
 * This function is the same as the C function OGR_G_BufferEx().
 *
 * @param dfDist the buffer distance to be applied. Should be expressed into
 *               the same unit as the coordinates of the geometry.
 * @param papszOptions NULL terminated list of options (may be NULL)
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 *
 * @since GDAL 3.10
 */

OGRGeometry *
OGRGeometry::BufferEx(UNUSED_IF_NO_GEOS double dfDist,
                      UNUSED_IF_NO_GEOS CSLConstList papszOptions) const
{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;

#else
    OGRGeometry *poOGRProduct = nullptr;
    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();

    auto hParams = GEOSBufferParams_create_r(hGEOSCtxt);
    bool bParamsAreValid = true;

    for (const auto &[pszParam, pszValue] : cpl::IterateNameValue(papszOptions))
    {
        if (EQUAL(pszParam, "ENDCAP_STYLE"))
        {
            int nStyle;
            if (EQUAL(pszValue, "ROUND"))
            {
                nStyle = GEOSBUF_CAP_ROUND;
            }
            else if (EQUAL(pszValue, "FLAT"))
            {
                nStyle = GEOSBUF_CAP_FLAT;
            }
            else if (EQUAL(pszValue, "SQUARE"))
            {
                nStyle = GEOSBUF_CAP_SQUARE;
            }
            else
            {
                bParamsAreValid = false;
                CPLError(CE_Failure, CPLE_NotSupported,
                         "Invalid value for ENDCAP_STYLE: %s", pszValue);
                break;
            }

            if (!GEOSBufferParams_setEndCapStyle_r(hGEOSCtxt, hParams, nStyle))
            {
                bParamsAreValid = false;
            }
        }
        else if (EQUAL(pszParam, "JOIN_STYLE"))
        {
            int nStyle;
            if (EQUAL(pszValue, "ROUND"))
            {
                nStyle = GEOSBUF_JOIN_ROUND;
            }
            else if (EQUAL(pszValue, "MITRE"))
            {
                nStyle = GEOSBUF_JOIN_MITRE;
            }
            else if (EQUAL(pszValue, "BEVEL"))
            {
                nStyle = GEOSBUF_JOIN_BEVEL;
            }
            else
            {
                bParamsAreValid = false;
                CPLError(CE_Failure, CPLE_NotSupported,
                         "Invalid value for JOIN_STYLE: %s", pszValue);
                break;
            }

            if (!GEOSBufferParams_setJoinStyle_r(hGEOSCtxt, hParams, nStyle))
            {
                bParamsAreValid = false;
                break;
            }
        }
        else if (EQUAL(pszParam, "MITRE_LIMIT"))
        {
            try
            {
                std::size_t end;
                double dfLimit = std::stod(pszValue, &end);

                if (end != strlen(pszValue))
                {
                    throw std::invalid_argument("");
                }

                if (!GEOSBufferParams_setMitreLimit_r(hGEOSCtxt, hParams,
                                                      dfLimit))
                {
                    bParamsAreValid = false;
                    break;
                }
            }
            catch (const std::invalid_argument &)
            {
                bParamsAreValid = false;
                CPLError(CE_Failure, CPLE_IllegalArg,
                         "Invalid value for MITRE_LIMIT: %s", pszValue);
            }
            catch (const std::out_of_range &)
            {
                bParamsAreValid = false;
                CPLError(CE_Failure, CPLE_IllegalArg,
                         "Invalid value for MITRE_LIMIT: %s", pszValue);
            }
        }
        else if (EQUAL(pszParam, "QUADRANT_SEGMENTS"))
        {
            try
            {
                std::size_t end;
                int nQuadSegs = std::stoi(pszValue, &end, 10);

                if (end != strlen(pszValue))
                {
                    throw std::invalid_argument("");
                }

                if (!GEOSBufferParams_setQuadrantSegments_r(hGEOSCtxt, hParams,
                                                            nQuadSegs))
                {
                    bParamsAreValid = false;
                    break;
                }
            }
            catch (const std::invalid_argument &)
            {
                bParamsAreValid = false;
                CPLError(CE_Failure, CPLE_IllegalArg,
                         "Invalid value for QUADRANT_SEGMENTS: %s", pszValue);
            }
            catch (const std::out_of_range &)
            {
                bParamsAreValid = false;
                CPLError(CE_Failure, CPLE_IllegalArg,
                         "Invalid value for QUADRANT_SEGMENTS: %s", pszValue);
            }
        }
        else if (EQUAL(pszParam, "SINGLE_SIDED"))
        {
            bool bSingleSided = CPLTestBool(pszValue);

            if (!GEOSBufferParams_setSingleSided_r(hGEOSCtxt, hParams,
                                                   bSingleSided))
            {
                bParamsAreValid = false;
                break;
            }
        }
        else
        {
            bParamsAreValid = false;
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Unsupported buffer option: %s", pszValue);
        }
    }

    if (bParamsAreValid)
    {
        GEOSGeom hGeosGeom = exportToGEOS(hGEOSCtxt);
        if (hGeosGeom != nullptr)
        {
            GEOSGeom hGeosProduct =
                GEOSBufferWithParams_r(hGEOSCtxt, hGeosGeom, hParams, dfDist);
            GEOSGeom_destroy_r(hGEOSCtxt, hGeosGeom);

            if (hGeosProduct != nullptr)
            {
                poOGRProduct = BuildGeometryFromGEOS(hGEOSCtxt, hGeosProduct,
                                                     this, nullptr);
            }
        }
    }

    GEOSBufferParams_destroy_r(hGEOSCtxt, hParams);
    freeGEOSContext(hGEOSCtxt);
    return poOGRProduct;
#endif
}

/**
 * \brief Compute buffer of geometry.
 *
 * Builds a new geometry containing the buffer region around the geometry
 * on which it is invoked.  The buffer is a polygon containing the region within
 * the buffer distance of the original geometry.
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * The following options are supported. See the GEOS library for more detailed
 * descriptions.
 *
 * <ul>
 * <li>ENDCAP_STYLE=ROUND/FLAT/SQUARE</li>
 * <li>JOIN_STYLE=ROUND/MITRE/BEVEL</li>
 * <li>MITRE_LIMIT=double</li>
 * <li>QUADRANT_SEGMENTS=int</li>
 * <li>SINGLE_SIDED=YES/NO</li>
 * </ul>
 *
 * This function is the same as the C++ method OGRGeometry::BufferEx().
 *
 * @param hTarget the geometry.
 * @param dfDist the buffer distance to be applied. Should be expressed into
 *               the same unit as the coordinates of the geometry.
 * @param papszOptions NULL terminated list of options (may be NULL)
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 *
 * @since GDAL 3.10
 */

OGRGeometryH OGR_G_BufferEx(OGRGeometryH hTarget, double dfDist,
                            CSLConstList papszOptions)

{
    VALIDATE_POINTER1(hTarget, "OGR_G_BufferEx", nullptr);

    return OGRGeometry::ToHandle(
        OGRGeometry::FromHandle(hTarget)->BufferEx(dfDist, papszOptions));
}

/************************************************************************/
/*                            Intersection()                            */
/************************************************************************/

/**
 * \brief Compute intersection.
 *
 * Generates a new geometry which is the region of intersection of the
 * two geometries operated on.  The Intersects() method can be used to test if
 * two geometries intersect.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This method is the same as the C function OGR_G_Intersection().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param poOtherGeom the other geometry intersected with "this" geometry.
 *
 * @return a new geometry to be freed by the caller, or NULL if there is no
 * intersection or if an error occurs.
 *
 */

OGRGeometry *
OGRGeometry::Intersection(UNUSED_PARAMETER const OGRGeometry *poOtherGeom) const

{
    if (IsSFCGALCompatible() || poOtherGeom->IsSFCGALCompatible())
    {
#ifndef HAVE_SFCGAL

        CPLError(CE_Failure, CPLE_NotSupported, "SFCGAL support not enabled.");
        return nullptr;

#else

        sfcgal_geometry_t *poThis = OGRGeometry::OGRexportToSFCGAL(this);
        if (poThis == nullptr)
            return nullptr;

        sfcgal_geometry_t *poOther =
            OGRGeometry::OGRexportToSFCGAL(poOtherGeom);
        if (poOther == nullptr)
        {
            sfcgal_geometry_delete(poThis);
            return nullptr;
        }

        sfcgal_geometry_t *poRes =
            sfcgal_geometry_intersection_3d(poThis, poOther);
        OGRGeometry *h_prodGeom = SFCGALexportToOGR(poRes);
        if (h_prodGeom != nullptr && getSpatialReference() != nullptr &&
            poOtherGeom->getSpatialReference() != nullptr &&
            poOtherGeom->getSpatialReference()->IsSame(getSpatialReference()))
            h_prodGeom->assignSpatialReference(getSpatialReference());

        sfcgal_geometry_delete(poThis);
        sfcgal_geometry_delete(poOther);
        sfcgal_geometry_delete(poRes);

        return h_prodGeom;

#endif
    }

    else
    {
#ifndef HAVE_GEOS

        CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
        return nullptr;

#else
        return BuildGeometryFromTwoGeoms(this, poOtherGeom, GEOSIntersection_r);
#endif /* HAVE_GEOS */
    }
}

/************************************************************************/
/*                         OGR_G_Intersection()                         */
/************************************************************************/

/**
 * \brief Compute intersection.
 *
 * Generates a new geometry which is the region of intersection of the
 * two geometries operated on.  The OGR_G_Intersects() function can be used to
 * test if two geometries intersect.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This function is the same as the C++ method OGRGeometry::Intersection().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry.
 * @param hOther the other geometry.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if there is not intersection of if an error occurs.
 */

OGRGeometryH OGR_G_Intersection(OGRGeometryH hThis, OGRGeometryH hOther)

{
    VALIDATE_POINTER1(hThis, "OGR_G_Intersection", nullptr);

    return OGRGeometry::ToHandle(OGRGeometry::FromHandle(hThis)->Intersection(
        OGRGeometry::FromHandle(hOther)));
}

/************************************************************************/
/*                               Union()                                */
/************************************************************************/

/**
 * \brief Compute union.
 *
 * Generates a new geometry which is the region of union of the
 * two geometries operated on.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This method is the same as the C function OGR_G_Union().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param poOtherGeom the other geometry unioned with "this" geometry.
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 */

OGRGeometry *
OGRGeometry::Union(UNUSED_PARAMETER const OGRGeometry *poOtherGeom) const

{
    if (IsSFCGALCompatible() || poOtherGeom->IsSFCGALCompatible())
    {
#ifndef HAVE_SFCGAL

        CPLError(CE_Failure, CPLE_NotSupported, "SFCGAL support not enabled.");
        return nullptr;

#else

        sfcgal_geometry_t *poThis = OGRGeometry::OGRexportToSFCGAL(this);
        if (poThis == nullptr)
            return nullptr;

        sfcgal_geometry_t *poOther =
            OGRGeometry::OGRexportToSFCGAL(poOtherGeom);
        if (poOther == nullptr)
        {
            sfcgal_geometry_delete(poThis);
            return nullptr;
        }

        sfcgal_geometry_t *poRes = sfcgal_geometry_union_3d(poThis, poOther);
        OGRGeometry *h_prodGeom = OGRGeometry::SFCGALexportToOGR(poRes);
        if (h_prodGeom != nullptr && getSpatialReference() != nullptr &&
            poOtherGeom->getSpatialReference() != nullptr &&
            poOtherGeom->getSpatialReference()->IsSame(getSpatialReference()))
            h_prodGeom->assignSpatialReference(getSpatialReference());

        sfcgal_geometry_delete(poThis);
        sfcgal_geometry_delete(poOther);
        sfcgal_geometry_delete(poRes);

        return h_prodGeom;

#endif
    }

    else
    {
#ifndef HAVE_GEOS

        CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
        return nullptr;

#else
        return BuildGeometryFromTwoGeoms(this, poOtherGeom, GEOSUnion_r);
#endif /* HAVE_GEOS */
    }
}

/************************************************************************/
/*                            OGR_G_Union()                             */
/************************************************************************/

/**
 * \brief Compute union.
 *
 * Generates a new geometry which is the region of union of the
 * two geometries operated on.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This function is the same as the C++ method OGRGeometry::Union().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry.
 * @param hOther the other geometry.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 */

OGRGeometryH OGR_G_Union(OGRGeometryH hThis, OGRGeometryH hOther)

{
    VALIDATE_POINTER1(hThis, "OGR_G_Union", nullptr);

    return OGRGeometry::ToHandle(
        OGRGeometry::FromHandle(hThis)->Union(OGRGeometry::FromHandle(hOther)));
}

/************************************************************************/
/*                               UnionCascaded()                        */
/************************************************************************/

/**
 * \brief Compute union using cascading.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * The input geometry must be a MultiPolygon.
 *
 * This method is the same as the C function OGR_G_UnionCascaded().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 *
 * @since OGR 1.8.0
 *
 * @deprecated Use UnaryUnion() instead
 */

OGRGeometry *OGRGeometry::UnionCascaded() const

{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;
#else

#if GEOS_VERSION_MAJOR == 3 && GEOS_VERSION_MINOR < 11
    if (wkbFlatten(getGeometryType()) == wkbMultiPolygon && IsEmpty())
    {
        // GEOS < 3.11 crashes on an empty multipolygon input
        auto poRet = new OGRGeometryCollection();
        poRet->assignSpatialReference(getSpatialReference());
        return poRet;
    }
#endif
    OGRGeometry *poOGRProduct = nullptr;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hThisGeosGeom = exportToGEOS(hGEOSCtxt);
    if (hThisGeosGeom != nullptr)
    {
        GEOSGeom hGeosProduct = GEOSUnionCascaded_r(hGEOSCtxt, hThisGeosGeom);
        GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);

        poOGRProduct =
            BuildGeometryFromGEOS(hGEOSCtxt, hGeosProduct, this, nullptr);
    }
    freeGEOSContext(hGEOSCtxt);

    return poOGRProduct;

#endif  // HAVE_GEOS
}

/************************************************************************/
/*                            OGR_G_UnionCascaded()                     */
/************************************************************************/

/**
 * \brief Compute union using cascading.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * The input geometry must be a MultiPolygon.
 *
 * This function is the same as the C++ method OGRGeometry::UnionCascaded().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 *
 * @deprecated Use OGR_G_UnaryUnion() instead
 */

OGRGeometryH OGR_G_UnionCascaded(OGRGeometryH hThis)

{
    VALIDATE_POINTER1(hThis, "OGR_G_UnionCascaded", nullptr);

    return OGRGeometry::ToHandle(
        OGRGeometry::FromHandle(hThis)->UnionCascaded());
}

/************************************************************************/
/*                               UnaryUnion()                           */
/************************************************************************/

/**
 * \brief Returns the union of all components of a single geometry.
 *
 * Usually used to convert a collection into the smallest set of polygons that
 * cover the same area.
 *
 * See https://postgis.net/docs/ST_UnaryUnion.html for more details.
 *
 * This method is the same as the C function OGR_G_UnaryUnion().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 *
 * @since GDAL 3.7
 */

OGRGeometry *OGRGeometry::UnaryUnion() const

{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;
#else

#if GEOS_VERSION_MAJOR == 3 && GEOS_VERSION_MINOR < 11
    if (IsEmpty())
    {
        // GEOS < 3.11 crashes on an empty geometry
        auto poRet = new OGRGeometryCollection();
        poRet->assignSpatialReference(getSpatialReference());
        return poRet;
    }
#endif
    OGRGeometry *poOGRProduct = nullptr;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hThisGeosGeom = exportToGEOS(hGEOSCtxt);
    if (hThisGeosGeom != nullptr)
    {
        GEOSGeom hGeosProduct = GEOSUnaryUnion_r(hGEOSCtxt, hThisGeosGeom);
        GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);

        poOGRProduct =
            BuildGeometryFromGEOS(hGEOSCtxt, hGeosProduct, this, nullptr);
    }
    freeGEOSContext(hGEOSCtxt);

    return poOGRProduct;

#endif  // HAVE_GEOS
}

/************************************************************************/
/*                            OGR_G_UnaryUnion()                        */
/************************************************************************/

/**
 * \brief Returns the union of all components of a single geometry.
 *
 * Usually used to convert a collection into the smallest set of polygons that
 * cover the same area.
 *
 * See https://postgis.net/docs/ST_UnaryUnion.html for more details.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This function is the same as the C++ method OGRGeometry::UnaryUnion().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 *
 * @since GDAL 3.7
 */

OGRGeometryH OGR_G_UnaryUnion(OGRGeometryH hThis)

{
    VALIDATE_POINTER1(hThis, "OGR_G_UnaryUnion", nullptr);

    return OGRGeometry::ToHandle(OGRGeometry::FromHandle(hThis)->UnaryUnion());
}

/************************************************************************/
/*                             Difference()                             */
/************************************************************************/

/**
 * \brief Compute difference.
 *
 * Generates a new geometry which is the region of this geometry with the
 * region of the second geometry removed.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This method is the same as the C function OGR_G_Difference().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param poOtherGeom the other geometry removed from "this" geometry.
 *
 * @return a new geometry to be freed by the caller, or NULL if the difference
 * is empty or if an error occurs.
 */

OGRGeometry *
OGRGeometry::Difference(UNUSED_PARAMETER const OGRGeometry *poOtherGeom) const

{
    if (IsSFCGALCompatible() || poOtherGeom->IsSFCGALCompatible())
    {
#ifndef HAVE_SFCGAL

        CPLError(CE_Failure, CPLE_NotSupported, "SFCGAL support not enabled.");
        return nullptr;

#else

        sfcgal_geometry_t *poThis = OGRGeometry::OGRexportToSFCGAL(this);
        if (poThis == nullptr)
            return nullptr;

        sfcgal_geometry_t *poOther =
            OGRGeometry::OGRexportToSFCGAL(poOtherGeom);
        if (poOther == nullptr)
        {
            sfcgal_geometry_delete(poThis);
            return nullptr;
        }

        sfcgal_geometry_t *poRes =
            sfcgal_geometry_difference_3d(poThis, poOther);
        OGRGeometry *h_prodGeom = OGRGeometry::SFCGALexportToOGR(poRes);
        if (h_prodGeom != nullptr && getSpatialReference() != nullptr &&
            poOtherGeom->getSpatialReference() != nullptr &&
            poOtherGeom->getSpatialReference()->IsSame(getSpatialReference()))
            h_prodGeom->assignSpatialReference(getSpatialReference());

        sfcgal_geometry_delete(poThis);
        sfcgal_geometry_delete(poOther);
        sfcgal_geometry_delete(poRes);

        return h_prodGeom;

#endif
    }

    else
    {
#ifndef HAVE_GEOS

        CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
        return nullptr;

#else
        return BuildGeometryFromTwoGeoms(this, poOtherGeom, GEOSDifference_r);
#endif /* HAVE_GEOS */
    }
}

/************************************************************************/
/*                          OGR_G_Difference()                          */
/************************************************************************/

/**
 * \brief Compute difference.
 *
 * Generates a new geometry which is the region of this geometry with the
 * region of the other geometry removed.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This function is the same as the C++ method OGRGeometry::Difference().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry.
 * @param hOther the other geometry.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if the difference is empty or if an error occurs.
 */

OGRGeometryH OGR_G_Difference(OGRGeometryH hThis, OGRGeometryH hOther)

{
    VALIDATE_POINTER1(hThis, "OGR_G_Difference", nullptr);

    return OGRGeometry::ToHandle(OGRGeometry::FromHandle(hThis)->Difference(
        OGRGeometry::FromHandle(hOther)));
}

/************************************************************************/
/*                        SymDifference()                               */
/************************************************************************/

/**
 * \brief Compute symmetric difference.
 *
 * Generates a new geometry which is the symmetric difference of this
 * geometry and the second geometry passed into the method.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This method is the same as the C function OGR_G_SymDifference().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param poOtherGeom the other geometry.
 *
 * @return a new geometry to be freed by the caller, or NULL if the difference
 * is empty or if an error occurs.
 *
 * @since OGR 1.8.0
 */

OGRGeometry *OGRGeometry::SymDifference(
    UNUSED_IF_NO_GEOS const OGRGeometry *poOtherGeom) const

{
    if (IsSFCGALCompatible() || poOtherGeom->IsSFCGALCompatible())
    {
#ifndef HAVE_SFCGAL
        CPLError(CE_Failure, CPLE_NotSupported, "SFCGAL support not enabled.");
        return nullptr;
#else
        OGRGeometry *poFirstDifference = Difference(poOtherGeom);
        if (poFirstDifference == nullptr)
            return nullptr;

        OGRGeometry *poOtherDifference = poOtherGeom->Difference(this);
        if (poOtherDifference == nullptr)
        {
            delete poFirstDifference;
            return nullptr;
        }

        OGRGeometry *poSymDiff = poFirstDifference->Union(poOtherDifference);
        delete poFirstDifference;
        delete poOtherDifference;
        return poSymDiff;
#endif
    }

#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;

#else
    return BuildGeometryFromTwoGeoms(this, poOtherGeom, GEOSSymDifference_r);
#endif  // HAVE_GEOS
}

//! @cond Doxygen_Suppress
/**
 * \brief Compute symmetric difference (deprecated)
 *
 * @deprecated
 *
 * @see OGRGeometry::SymDifference()
 */
OGRGeometry *
OGRGeometry::SymmetricDifference(const OGRGeometry *poOtherGeom) const

{
    return SymDifference(poOtherGeom);
}

//! @endcond

/************************************************************************/
/*                      OGR_G_SymDifference()                           */
/************************************************************************/

/**
 * \brief Compute symmetric difference.
 *
 * Generates a new geometry which is the symmetric difference of this
 * geometry and the other geometry.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This function is the same as the C++ method
 * OGRGeometry::SymmetricDifference().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry.
 * @param hOther the other geometry.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if the difference is empty or if an error occurs.
 *
 * @since OGR 1.8.0
 */

OGRGeometryH OGR_G_SymDifference(OGRGeometryH hThis, OGRGeometryH hOther)

{
    VALIDATE_POINTER1(hThis, "OGR_G_SymDifference", nullptr);

    return OGRGeometry::ToHandle(OGRGeometry::FromHandle(hThis)->SymDifference(
        OGRGeometry::FromHandle(hOther)));
}

/**
 * \brief Compute symmetric difference (deprecated)
 *
 * @deprecated
 *
 * @see OGR_G_SymmetricDifference()
 */
OGRGeometryH OGR_G_SymmetricDifference(OGRGeometryH hThis, OGRGeometryH hOther)

{
    VALIDATE_POINTER1(hThis, "OGR_G_SymmetricDifference", nullptr);

    return OGRGeometry::ToHandle(OGRGeometry::FromHandle(hThis)->SymDifference(
        OGRGeometry::FromHandle(hOther)));
}

/************************************************************************/
/*                              Disjoint()                              */
/************************************************************************/

/**
 * \brief Test for disjointness.
 *
 * Tests if this geometry and the other passed into the method are disjoint.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This method is the same as the C function OGR_G_Disjoint().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param poOtherGeom the geometry to compare to this geometry.
 *
 * @return TRUE if they are disjoint, otherwise FALSE.
 */

OGRBoolean
OGRGeometry::Disjoint(UNUSED_IF_NO_GEOS const OGRGeometry *poOtherGeom) const

{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return FALSE;

#else
    return OGRGEOSBooleanPredicate(this, poOtherGeom, GEOSDisjoint_r);
#endif  // HAVE_GEOS
}

/************************************************************************/
/*                           OGR_G_Disjoint()                           */
/************************************************************************/

/**
 * \brief Test for disjointness.
 *
 * Tests if this geometry and the other geometry are disjoint.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This function is the same as the C++ method OGRGeometry::Disjoint().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry to compare.
 * @param hOther the other geometry to compare.
 *
 * @return TRUE if they are disjoint, otherwise FALSE.
 */
int OGR_G_Disjoint(OGRGeometryH hThis, OGRGeometryH hOther)

{
    VALIDATE_POINTER1(hThis, "OGR_G_Disjoint", FALSE);

    return OGRGeometry::FromHandle(hThis)->Disjoint(
        OGRGeometry::FromHandle(hOther));
}

/************************************************************************/
/*                              Touches()                               */
/************************************************************************/

/**
 * \brief Test for touching.
 *
 * Tests if this geometry and the other passed into the method are touching.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This method is the same as the C function OGR_G_Touches().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param poOtherGeom the geometry to compare to this geometry.
 *
 * @return TRUE if they are touching, otherwise FALSE.
 */

OGRBoolean
OGRGeometry::Touches(UNUSED_IF_NO_GEOS const OGRGeometry *poOtherGeom) const

{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return FALSE;

#else
    return OGRGEOSBooleanPredicate(this, poOtherGeom, GEOSTouches_r);
#endif  // HAVE_GEOS
}

/************************************************************************/
/*                           OGR_G_Touches()                            */
/************************************************************************/
/**
 * \brief Test for touching.
 *
 * Tests if this geometry and the other geometry are touching.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This function is the same as the C++ method OGRGeometry::Touches().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry to compare.
 * @param hOther the other geometry to compare.
 *
 * @return TRUE if they are touching, otherwise FALSE.
 */

int OGR_G_Touches(OGRGeometryH hThis, OGRGeometryH hOther)

{
    VALIDATE_POINTER1(hThis, "OGR_G_Touches", FALSE);

    return OGRGeometry::FromHandle(hThis)->Touches(
        OGRGeometry::FromHandle(hOther));
}

/************************************************************************/
/*                              Crosses()                               */
/************************************************************************/

/**
 * \brief Test for crossing.
 *
 * Tests if this geometry and the other passed into the method are crossing.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This method is the same as the C function OGR_G_Crosses().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param poOtherGeom the geometry to compare to this geometry.
 *
 * @return TRUE if they are crossing, otherwise FALSE.
 */

OGRBoolean
OGRGeometry::Crosses(UNUSED_PARAMETER const OGRGeometry *poOtherGeom) const

{
    if (IsSFCGALCompatible() || poOtherGeom->IsSFCGALCompatible())
    {
#ifndef HAVE_SFCGAL

        CPLError(CE_Failure, CPLE_NotSupported, "SFCGAL support not enabled.");
        return FALSE;

#else

        sfcgal_geometry_t *poThis = OGRGeometry::OGRexportToSFCGAL(this);
        if (poThis == nullptr)
            return FALSE;

        sfcgal_geometry_t *poOther =
            OGRGeometry::OGRexportToSFCGAL(poOtherGeom);
        if (poOther == nullptr)
        {
            sfcgal_geometry_delete(poThis);
            return FALSE;
        }

        int res = sfcgal_geometry_intersects_3d(poThis, poOther);

        sfcgal_geometry_delete(poThis);
        sfcgal_geometry_delete(poOther);

        return (res == 1) ? TRUE : FALSE;

#endif
    }

    else
    {

#ifndef HAVE_GEOS

        CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
        return FALSE;

#else
        return OGRGEOSBooleanPredicate(this, poOtherGeom, GEOSCrosses_r);
#endif /* HAVE_GEOS */
    }
}

/************************************************************************/
/*                           OGR_G_Crosses()                            */
/************************************************************************/
/**
 * \brief Test for crossing.
 *
 * Tests if this geometry and the other geometry are crossing.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This function is the same as the C++ method OGRGeometry::Crosses().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry to compare.
 * @param hOther the other geometry to compare.
 *
 * @return TRUE if they are crossing, otherwise FALSE.
 */

int OGR_G_Crosses(OGRGeometryH hThis, OGRGeometryH hOther)

{
    VALIDATE_POINTER1(hThis, "OGR_G_Crosses", FALSE);

    return OGRGeometry::FromHandle(hThis)->Crosses(
        OGRGeometry::FromHandle(hOther));
}

/************************************************************************/
/*                               Within()                               */
/************************************************************************/

/**
 * \brief Test for containment.
 *
 * Tests if actual geometry object is within the passed geometry.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This method is the same as the C function OGR_G_Within().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param poOtherGeom the geometry to compare to this geometry.
 *
 * @return TRUE if poOtherGeom is within this geometry, otherwise FALSE.
 */

OGRBoolean
OGRGeometry::Within(UNUSED_IF_NO_GEOS const OGRGeometry *poOtherGeom) const

{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return FALSE;

#else
    return OGRGEOSBooleanPredicate(this, poOtherGeom, GEOSWithin_r);
#endif  // HAVE_GEOS
}

/************************************************************************/
/*                            OGR_G_Within()                            */
/************************************************************************/

/**
 * \brief Test for containment.
 *
 * Tests if this geometry is within the other geometry.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This function is the same as the C++ method OGRGeometry::Within().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry to compare.
 * @param hOther the other geometry to compare.
 *
 * @return TRUE if hThis is within hOther, otherwise FALSE.
 */
int OGR_G_Within(OGRGeometryH hThis, OGRGeometryH hOther)

{
    VALIDATE_POINTER1(hThis, "OGR_G_Within", FALSE);

    return OGRGeometry::FromHandle(hThis)->Within(
        OGRGeometry::FromHandle(hOther));
}

/************************************************************************/
/*                              Contains()                              */
/************************************************************************/

/**
 * \brief Test for containment.
 *
 * Tests if actual geometry object contains the passed geometry.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This method is the same as the C function OGR_G_Contains().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param poOtherGeom the geometry to compare to this geometry.
 *
 * @return TRUE if poOtherGeom contains this geometry, otherwise FALSE.
 */

OGRBoolean
OGRGeometry::Contains(UNUSED_IF_NO_GEOS const OGRGeometry *poOtherGeom) const

{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return FALSE;

#else
    return OGRGEOSBooleanPredicate(this, poOtherGeom, GEOSContains_r);
#endif  // HAVE_GEOS
}

/************************************************************************/
/*                            OGR_G_Contains()                            */
/************************************************************************/

/**
 * \brief Test for containment.
 *
 * Tests if this geometry contains the other geometry.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This function is the same as the C++ method OGRGeometry::Contains().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry to compare.
 * @param hOther the other geometry to compare.
 *
 * @return TRUE if hThis contains hOther geometry, otherwise FALSE.
 */
int OGR_G_Contains(OGRGeometryH hThis, OGRGeometryH hOther)

{
    VALIDATE_POINTER1(hThis, "OGR_G_Contains", FALSE);

    return OGRGeometry::FromHandle(hThis)->Contains(
        OGRGeometry::FromHandle(hOther));
}

/************************************************************************/
/*                              Overlaps()                              */
/************************************************************************/

/**
 * \brief Test for overlap.
 *
 * Tests if this geometry and the other passed into the method overlap, that is
 * their intersection has a non-zero area.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This method is the same as the C function OGR_G_Overlaps().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param poOtherGeom the geometry to compare to this geometry.
 *
 * @return TRUE if they are overlapping, otherwise FALSE.
 */

OGRBoolean
OGRGeometry::Overlaps(UNUSED_IF_NO_GEOS const OGRGeometry *poOtherGeom) const

{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return FALSE;

#else
    return OGRGEOSBooleanPredicate(this, poOtherGeom, GEOSOverlaps_r);
#endif  // HAVE_GEOS
}

/************************************************************************/
/*                           OGR_G_Overlaps()                           */
/************************************************************************/
/**
 * \brief Test for overlap.
 *
 * Tests if this geometry and the other geometry overlap, that is their
 * intersection has a non-zero area.
 *
 * Geometry validity is not checked. In case you are unsure of the validity
 * of the input geometries, call IsValid() before, otherwise the result might
 * be wrong.
 *
 * This function is the same as the C++ method OGRGeometry::Overlaps().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry to compare.
 * @param hOther the other geometry to compare.
 *
 * @return TRUE if they are overlapping, otherwise FALSE.
 */

int OGR_G_Overlaps(OGRGeometryH hThis, OGRGeometryH hOther)

{
    VALIDATE_POINTER1(hThis, "OGR_G_Overlaps", FALSE);

    return OGRGeometry::FromHandle(hThis)->Overlaps(
        OGRGeometry::FromHandle(hOther));
}

/************************************************************************/
/*                             closeRings()                             */
/************************************************************************/

/**
 * \brief Force rings to be closed.
 *
 * If this geometry, or any contained geometries has polygon rings that
 * are not closed, they will be closed by adding the starting point at
 * the end.
 */

void OGRGeometry::closeRings()
{
}

/************************************************************************/
/*                          OGR_G_CloseRings()                          */
/************************************************************************/

/**
 * \brief Force rings to be closed.
 *
 * If this geometry, or any contained geometries has polygon rings that
 * are not closed, they will be closed by adding the starting point at
 * the end.
 *
 * @param hGeom handle to the geometry.
 */

void OGR_G_CloseRings(OGRGeometryH hGeom)

{
    VALIDATE_POINTER0(hGeom, "OGR_G_CloseRings");

    OGRGeometry::FromHandle(hGeom)->closeRings();
}

/************************************************************************/
/*                              Centroid()                              */
/************************************************************************/

/**
 * \brief Compute the geometry centroid.
 *
 * The centroid location is applied to the passed in OGRPoint object.
 * The centroid is not necessarily within the geometry.
 *
 * This method relates to the SFCOM ISurface::get_Centroid() method
 * however the current implementation based on GEOS can operate on other
 * geometry types such as multipoint, linestring, geometrycollection such as
 * multipolygons.
 * OGC SF SQL 1.1 defines the operation for surfaces (polygons).
 * SQL/MM-Part 3 defines the operation for surfaces and multisurfaces
 * (multipolygons).
 *
 * This function is the same as the C function OGR_G_Centroid().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @return OGRERR_NONE on success or OGRERR_FAILURE on error.
 *
 * @since OGR 1.8.0 as a OGRGeometry method (previously was restricted
 * to OGRPolygon)
 */

OGRErr OGRGeometry::Centroid(OGRPoint *poPoint) const

{
    if (poPoint == nullptr)
        return OGRERR_FAILURE;

#ifndef HAVE_GEOS
    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return OGRERR_FAILURE;

#else

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hThisGeosGeom =
        exportToGEOS(hGEOSCtxt, /* bRemoveEmptyParts = */ true);

    if (hThisGeosGeom != nullptr)
    {
        GEOSGeom hOtherGeosGeom = GEOSGetCentroid_r(hGEOSCtxt, hThisGeosGeom);
        GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);

        if (hOtherGeosGeom == nullptr)
        {
            freeGEOSContext(hGEOSCtxt);
            return OGRERR_FAILURE;
        }

        OGRGeometry *poCentroidGeom =
            OGRGeometryFactory::createFromGEOS(hGEOSCtxt, hOtherGeosGeom);

        GEOSGeom_destroy_r(hGEOSCtxt, hOtherGeosGeom);

        if (poCentroidGeom == nullptr)
        {
            freeGEOSContext(hGEOSCtxt);
            return OGRERR_FAILURE;
        }
        if (wkbFlatten(poCentroidGeom->getGeometryType()) != wkbPoint)
        {
            delete poCentroidGeom;
            freeGEOSContext(hGEOSCtxt);
            return OGRERR_FAILURE;
        }

        if (getSpatialReference() != nullptr)
            poCentroidGeom->assignSpatialReference(getSpatialReference());

        OGRPoint *poCentroid = poCentroidGeom->toPoint();

        if (!poCentroid->IsEmpty())
        {
            poPoint->setX(poCentroid->getX());
            poPoint->setY(poCentroid->getY());
        }
        else
        {
            poPoint->empty();
        }

        delete poCentroidGeom;

        freeGEOSContext(hGEOSCtxt);
        return OGRERR_NONE;
    }
    else
    {
        freeGEOSContext(hGEOSCtxt);
        return OGRERR_FAILURE;
    }

#endif  // HAVE_GEOS
}

/************************************************************************/
/*                           OGR_G_Centroid()                           */
/************************************************************************/

/**
 * \brief Compute the geometry centroid.
 *
 * The centroid location is applied to the passed in OGRPoint object.
 * The centroid is not necessarily within the geometry.
 *
 * This method relates to the SFCOM ISurface::get_Centroid() method
 * however the current implementation based on GEOS can operate on other
 * geometry types such as multipoint, linestring, geometrycollection such as
 * multipolygons.
 * OGC SF SQL 1.1 defines the operation for surfaces (polygons).
 * SQL/MM-Part 3 defines the operation for surfaces and multisurfaces
 * (multipolygons).
 *
 * This function is the same as the C++ method OGRGeometry::Centroid().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @return OGRERR_NONE on success or OGRERR_FAILURE on error.
 */

int OGR_G_Centroid(OGRGeometryH hGeom, OGRGeometryH hCentroidPoint)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_Centroid", OGRERR_FAILURE);

    OGRGeometry *poCentroidGeom = OGRGeometry::FromHandle(hCentroidPoint);
    if (poCentroidGeom == nullptr)
        return OGRERR_FAILURE;
    if (wkbFlatten(poCentroidGeom->getGeometryType()) != wkbPoint)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Passed wrong geometry type as centroid argument.");
        return OGRERR_FAILURE;
    }

    return OGRGeometry::FromHandle(hGeom)->Centroid(poCentroidGeom->toPoint());
}

/************************************************************************/
/*                        OGR_G_PointOnSurface()                        */
/************************************************************************/

/**
 * \brief Returns a point guaranteed to lie on the surface.
 *
 * This method relates to the SFCOM ISurface::get_PointOnSurface() method
 * however the current implementation based on GEOS can operate on other
 * geometry types than the types that are supported by SQL/MM-Part 3 :
 * surfaces (polygons) and multisurfaces (multipolygons).
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hGeom the geometry to operate on.
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 *
 * @since OGR 1.10
 */

OGRGeometryH OGR_G_PointOnSurface(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_PointOnSurface", nullptr);

#ifndef HAVE_GEOS
    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;
#else

    OGRGeometry *poThis = OGRGeometry::FromHandle(hGeom);

    GEOSContextHandle_t hGEOSCtxt = OGRGeometry::createGEOSContext();
    GEOSGeom hThisGeosGeom = poThis->exportToGEOS(hGEOSCtxt);

    if (hThisGeosGeom != nullptr)
    {
        GEOSGeom hOtherGeosGeom =
            GEOSPointOnSurface_r(hGEOSCtxt, hThisGeosGeom);
        GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);

        if (hOtherGeosGeom == nullptr)
        {
            OGRGeometry::freeGEOSContext(hGEOSCtxt);
            return nullptr;
        }

        OGRGeometry *poInsidePointGeom =
            OGRGeometryFactory::createFromGEOS(hGEOSCtxt, hOtherGeosGeom);

        GEOSGeom_destroy_r(hGEOSCtxt, hOtherGeosGeom);

        if (poInsidePointGeom == nullptr)
        {
            OGRGeometry::freeGEOSContext(hGEOSCtxt);
            return nullptr;
        }
        if (wkbFlatten(poInsidePointGeom->getGeometryType()) != wkbPoint)
        {
            delete poInsidePointGeom;
            OGRGeometry::freeGEOSContext(hGEOSCtxt);
            return nullptr;
        }

        if (poThis->getSpatialReference() != nullptr)
            poInsidePointGeom->assignSpatialReference(
                poThis->getSpatialReference());

        OGRGeometry::freeGEOSContext(hGEOSCtxt);
        return OGRGeometry::ToHandle(poInsidePointGeom);
    }

    OGRGeometry::freeGEOSContext(hGEOSCtxt);
    return nullptr;
#endif
}

/************************************************************************/
/*                          PointOnSurfaceInternal()                    */
/************************************************************************/

//! @cond Doxygen_Suppress
OGRErr OGRGeometry::PointOnSurfaceInternal(OGRPoint *poPoint) const
{
    if (poPoint == nullptr || poPoint->IsEmpty())
        return OGRERR_FAILURE;

    OGRGeometryH hInsidePoint = OGR_G_PointOnSurface(
        OGRGeometry::ToHandle(const_cast<OGRGeometry *>(this)));
    if (hInsidePoint == nullptr)
        return OGRERR_FAILURE;

    OGRPoint *poInsidePoint = OGRGeometry::FromHandle(hInsidePoint)->toPoint();
    if (poInsidePoint->IsEmpty())
    {
        poPoint->empty();
    }
    else
    {
        poPoint->setX(poInsidePoint->getX());
        poPoint->setY(poInsidePoint->getY());
    }

    OGR_G_DestroyGeometry(hInsidePoint);

    return OGRERR_NONE;
}

//! @endcond

/************************************************************************/
/*                              Simplify()                              */
/************************************************************************/

/**
 * \brief Simplify the geometry.
 *
 * This function is the same as the C function OGR_G_Simplify().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param dTolerance the distance tolerance for the simplification.
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 *
 * @since OGR 1.8.0
 */

OGRGeometry *OGRGeometry::Simplify(UNUSED_IF_NO_GEOS double dTolerance) const

{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;

#else
    OGRGeometry *poOGRProduct = nullptr;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hThisGeosGeom = exportToGEOS(hGEOSCtxt);
    if (hThisGeosGeom != nullptr)
    {
        GEOSGeom hGeosProduct =
            GEOSSimplify_r(hGEOSCtxt, hThisGeosGeom, dTolerance);
        GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);
        poOGRProduct =
            BuildGeometryFromGEOS(hGEOSCtxt, hGeosProduct, this, nullptr);
    }
    freeGEOSContext(hGEOSCtxt);
    return poOGRProduct;

#endif  // HAVE_GEOS
}

/************************************************************************/
/*                         OGR_G_Simplify()                             */
/************************************************************************/

/**
 * \brief Compute a simplified geometry.
 *
 * This function is the same as the C++ method OGRGeometry::Simplify().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry.
 * @param dTolerance the distance tolerance for the simplification.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 *
 * @since OGR 1.8.0
 */

OGRGeometryH OGR_G_Simplify(OGRGeometryH hThis, double dTolerance)

{
    VALIDATE_POINTER1(hThis, "OGR_G_Simplify", nullptr);
    return OGRGeometry::ToHandle(
        OGRGeometry::FromHandle(hThis)->Simplify(dTolerance));
}

/************************************************************************/
/*                         SimplifyPreserveTopology()                   */
/************************************************************************/

/**
 * \brief Simplify the geometry while preserving topology.
 *
 * This function is the same as the C function OGR_G_SimplifyPreserveTopology().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param dTolerance the distance tolerance for the simplification.
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 *
 * @since OGR 1.9.0
 */

OGRGeometry *
OGRGeometry::SimplifyPreserveTopology(UNUSED_IF_NO_GEOS double dTolerance) const

{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;

#else
    OGRGeometry *poOGRProduct = nullptr;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hThisGeosGeom = exportToGEOS(hGEOSCtxt);
    if (hThisGeosGeom != nullptr)
    {
        GEOSGeom hGeosProduct = GEOSTopologyPreserveSimplify_r(
            hGEOSCtxt, hThisGeosGeom, dTolerance);
        GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);
        poOGRProduct =
            BuildGeometryFromGEOS(hGEOSCtxt, hGeosProduct, this, nullptr);
    }
    freeGEOSContext(hGEOSCtxt);
    return poOGRProduct;

#endif  // HAVE_GEOS
}

/************************************************************************/
/*                     OGR_G_SimplifyPreserveTopology()                 */
/************************************************************************/

/**
 * \brief Simplify the geometry while preserving topology.
 *
 * This function is the same as the C++ method
 * OGRGeometry::SimplifyPreserveTopology().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry.
 * @param dTolerance the distance tolerance for the simplification.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 *
 * @since OGR 1.9.0
 */

OGRGeometryH OGR_G_SimplifyPreserveTopology(OGRGeometryH hThis,
                                            double dTolerance)

{
    VALIDATE_POINTER1(hThis, "OGR_G_SimplifyPreserveTopology", nullptr);
    return OGRGeometry::ToHandle(
        OGRGeometry::FromHandle(hThis)->SimplifyPreserveTopology(dTolerance));
}

/************************************************************************/
/*                           roundCoordinates()                         */
/************************************************************************/

/** Round coordinates of the geometry to the specified precision.
 *
 * Note that this is not the same as OGRGeometry::SetPrecision(). The later
 * will return valid geometries, whereas roundCoordinates() does not make
 * such guarantee and may return geometries with invalidities, if they are
 * not compatible of the specified precision. roundCoordinates() supports
 * curve geometries, whereas SetPrecision() does not currently.
 *
 * One use case for roundCoordinates() is to undo the effect of
 * quantizeCoordinates().
 *
 * @param sPrecision Contains the precision requirements.
 * @since GDAL 3.9
 */
void OGRGeometry::roundCoordinates(const OGRGeomCoordinatePrecision &sPrecision)
{
    struct Rounder : public OGRDefaultGeometryVisitor
    {
        const OGRGeomCoordinatePrecision &m_precision;
        const double m_invXYResolution;
        const double m_invZResolution;
        const double m_invMResolution;

        explicit Rounder(const OGRGeomCoordinatePrecision &sPrecisionIn)
            : m_precision(sPrecisionIn),
              m_invXYResolution(m_precision.dfXYResolution !=
                                        OGRGeomCoordinatePrecision::UNKNOWN
                                    ? 1.0 / m_precision.dfXYResolution
                                    : 0.0),
              m_invZResolution(m_precision.dfZResolution !=
                                       OGRGeomCoordinatePrecision::UNKNOWN
                                   ? 1.0 / m_precision.dfZResolution
                                   : 0.0),
              m_invMResolution(m_precision.dfMResolution !=
                                       OGRGeomCoordinatePrecision::UNKNOWN
                                   ? 1.0 / m_precision.dfMResolution
                                   : 0.0)
        {
        }

        using OGRDefaultGeometryVisitor::visit;

        void visit(OGRPoint *poPoint) override
        {
            if (m_precision.dfXYResolution !=
                OGRGeomCoordinatePrecision::UNKNOWN)
            {
                poPoint->setX(std::round(poPoint->getX() * m_invXYResolution) *
                              m_precision.dfXYResolution);
                poPoint->setY(std::round(poPoint->getY() * m_invXYResolution) *
                              m_precision.dfXYResolution);
            }
            if (m_precision.dfZResolution !=
                    OGRGeomCoordinatePrecision::UNKNOWN &&
                poPoint->Is3D())
            {
                poPoint->setZ(std::round(poPoint->getZ() * m_invZResolution) *
                              m_precision.dfZResolution);
            }
            if (m_precision.dfMResolution !=
                    OGRGeomCoordinatePrecision::UNKNOWN &&
                poPoint->IsMeasured())
            {
                poPoint->setM(std::round(poPoint->getM() * m_invMResolution) *
                              m_precision.dfMResolution);
            }
        }
    };

    Rounder rounder(sPrecision);
    accept(&rounder);
}

/************************************************************************/
/*                           SetPrecision()                             */
/************************************************************************/

/** Set the geometry's precision, rounding all its coordinates to the precision
 * grid, and making sure the geometry is still valid.
 *
 * This is a stronger version of roundCoordinates().
 *
 * Note that at time of writing GEOS does no supported curve geometries. So
 * currently if this function is called on such a geometry, OGR will first call
 * getLinearGeometry() on the input and getCurveGeometry() on the output, but
 * that it is unlikely to yield to the expected result.
 *
 * This function is the same as the C function OGR_G_SetPrecision().
 *
 * This function is built on the GEOSGeom_setPrecision_r() function of the
 * GEOS library. Check it for the definition of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param dfGridSize size of the precision grid, or 0 for FLOATING
 *                 precision.
 * @param nFlags The bitwise OR of zero, one or several of OGR_GEOS_PREC_NO_TOPO
 *               and OGR_GEOS_PREC_KEEP_COLLAPSED
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 *
 * @since GDAL 3.9
 */

OGRGeometry *OGRGeometry::SetPrecision(UNUSED_IF_NO_GEOS double dfGridSize,
                                       UNUSED_IF_NO_GEOS int nFlags) const
{
#ifndef HAVE_GEOS
    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;

#else
    OGRGeometry *poOGRProduct = nullptr;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hThisGeosGeom = exportToGEOS(hGEOSCtxt);
    if (hThisGeosGeom != nullptr)
    {
        GEOSGeom hGeosProduct = GEOSGeom_setPrecision_r(
            hGEOSCtxt, hThisGeosGeom, dfGridSize, nFlags);
        GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);
        poOGRProduct =
            BuildGeometryFromGEOS(hGEOSCtxt, hGeosProduct, this, nullptr);
    }
    freeGEOSContext(hGEOSCtxt);
    return poOGRProduct;

#endif  // HAVE_GEOS
}

/************************************************************************/
/*                         OGR_G_SetPrecision()                         */
/************************************************************************/

/** Set the geometry's precision, rounding all its coordinates to the precision
 * grid, and making sure the geometry is still valid.
 *
 * This is a stronger version of roundCoordinates().
 *
 * Note that at time of writing GEOS does no supported curve geometries. So
 * currently if this function is called on such a geometry, OGR will first call
 * getLinearGeometry() on the input and getCurveGeometry() on the output, but
 * that it is unlikely to yield to the expected result.
 *
 * This function is the same as the C++ method OGRGeometry::SetPrecision().
 *
 * This function is built on the GEOSGeom_setPrecision_r() function of the
 * GEOS library. Check it for the definition of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry.
 * @param dfGridSize size of the precision grid, or 0 for FLOATING
 *                 precision.
 * @param nFlags The bitwise OR of zero, one or several of OGR_GEOS_PREC_NO_TOPO
 *               and OGR_GEOS_PREC_KEEP_COLLAPSED
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 *
 * @since GDAL 3.9
 */
OGRGeometryH OGR_G_SetPrecision(OGRGeometryH hThis, double dfGridSize,
                                int nFlags)
{
    VALIDATE_POINTER1(hThis, "OGR_G_SetPrecision", nullptr);
    return OGRGeometry::ToHandle(
        OGRGeometry::FromHandle(hThis)->SetPrecision(dfGridSize, nFlags));
}

/************************************************************************/
/*                         DelaunayTriangulation()                      */
/************************************************************************/

/**
 * \brief Return a Delaunay triangulation of the vertices of the geometry.
 *
 * This function is the same as the C function OGR_G_DelaunayTriangulation().
 *
 * This function is built on the GEOS library, v3.4 or above.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param dfTolerance optional snapping tolerance to use for improved robustness
 * @param bOnlyEdges if TRUE, will return a MULTILINESTRING, otherwise it will
 *                   return a GEOMETRYCOLLECTION containing triangular POLYGONs.
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 *
 * @since OGR 2.1
 */

#ifndef HAVE_GEOS
OGRGeometry *OGRGeometry::DelaunayTriangulation(double /*dfTolerance*/,
                                                int /*bOnlyEdges*/) const
{
    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;
}
#else
OGRGeometry *OGRGeometry::DelaunayTriangulation(double dfTolerance,
                                                int bOnlyEdges) const
{
    OGRGeometry *poOGRProduct = nullptr;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hThisGeosGeom = exportToGEOS(hGEOSCtxt);
    if (hThisGeosGeom != nullptr)
    {
        GEOSGeom hGeosProduct = GEOSDelaunayTriangulation_r(
            hGEOSCtxt, hThisGeosGeom, dfTolerance, bOnlyEdges);
        GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);
        poOGRProduct =
            BuildGeometryFromGEOS(hGEOSCtxt, hGeosProduct, this, nullptr);
    }
    freeGEOSContext(hGEOSCtxt);
    return poOGRProduct;
}
#endif

/************************************************************************/
/*                     OGR_G_DelaunayTriangulation()                    */
/************************************************************************/

/**
 * \brief Return a Delaunay triangulation of the vertices of the geometry.
 *
 * This function is the same as the C++ method
 * OGRGeometry::DelaunayTriangulation().
 *
 * This function is built on the GEOS library, v3.4 or above.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hThis the geometry.
 * @param dfTolerance optional snapping tolerance to use for improved robustness
 * @param bOnlyEdges if TRUE, will return a MULTILINESTRING, otherwise it will
 *                   return a GEOMETRYCOLLECTION containing triangular POLYGONs.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 *
 * @since OGR 2.1
 */

OGRGeometryH OGR_G_DelaunayTriangulation(OGRGeometryH hThis, double dfTolerance,
                                         int bOnlyEdges)

{
    VALIDATE_POINTER1(hThis, "OGR_G_DelaunayTriangulation", nullptr);

    return OGRGeometry::ToHandle(
        OGRGeometry::FromHandle(hThis)->DelaunayTriangulation(dfTolerance,
                                                              bOnlyEdges));
}

/************************************************************************/
/*                             Polygonize()                             */
/************************************************************************/
/* Contributor: Alessandro Furieri, a.furieri@lqt.it                    */
/* Developed for Faunalia (http://www.faunalia.it) with funding from    */
/* Regione Toscana - Settore SISTEMA INFORMATIVO TERRITORIALE ED        */
/*                   AMBIENTALE                                         */
/************************************************************************/

/**
 * \brief Polygonizes a set of sparse edges.
 *
 * A new geometry object is created and returned containing a collection
 * of reassembled Polygons: NULL will be returned if the input collection
 * doesn't corresponds to a MultiLinestring, or when reassembling Edges
 * into Polygons is impossible due to topological inconsistencies.
 *
 * This method is the same as the C function OGR_G_Polygonize().
 *
 * This method is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @return a new geometry to be freed by the caller, or NULL if an error occurs.
 *
 * @since OGR 1.9.0
 */

OGRGeometry *OGRGeometry::Polygonize() const

{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;

#else

    const OGRGeometryCollection *poColl = nullptr;
    if (wkbFlatten(getGeometryType()) == wkbGeometryCollection ||
        wkbFlatten(getGeometryType()) == wkbMultiLineString)
        poColl = toGeometryCollection();
    else
        return nullptr;

    const int nCount = poColl->getNumGeometries();

    OGRGeometry *poPolygsOGRGeom = nullptr;
    bool bError = false;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();

    GEOSGeom *pahGeosGeomList = new GEOSGeom[nCount];
    for (int ig = 0; ig < nCount; ig++)
    {
        GEOSGeom hGeosGeom = nullptr;
        const OGRGeometry *poChild = poColl->getGeometryRef(ig);
        if (poChild == nullptr ||
            wkbFlatten(poChild->getGeometryType()) != wkbLineString)
            bError = true;
        else
        {
            hGeosGeom = poChild->exportToGEOS(hGEOSCtxt);
            if (hGeosGeom == nullptr)
                bError = true;
        }
        pahGeosGeomList[ig] = hGeosGeom;
    }

    if (!bError)
    {
        GEOSGeom hGeosPolygs =
            GEOSPolygonize_r(hGEOSCtxt, pahGeosGeomList, nCount);

        poPolygsOGRGeom =
            BuildGeometryFromGEOS(hGEOSCtxt, hGeosPolygs, this, nullptr);
    }

    for (int ig = 0; ig < nCount; ig++)
    {
        GEOSGeom hGeosGeom = pahGeosGeomList[ig];
        if (hGeosGeom != nullptr)
            GEOSGeom_destroy_r(hGEOSCtxt, hGeosGeom);
    }
    delete[] pahGeosGeomList;
    freeGEOSContext(hGEOSCtxt);

    return poPolygsOGRGeom;

#endif  // HAVE_GEOS
}

/************************************************************************/
/*                          OGR_G_Polygonize()                          */
/************************************************************************/
/**
 * \brief Polygonizes a set of sparse edges.
 *
 * A new geometry object is created and returned containing a collection
 * of reassembled Polygons: NULL will be returned if the input collection
 * doesn't corresponds to a MultiLinestring, or when reassembling Edges
 * into Polygons is impossible due to topological inconsistencies.
 *
 * This function is the same as the C++ method OGRGeometry::Polygonize().
 *
 * This function is built on the GEOS library, check it for the definition
 * of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hTarget The Geometry to be polygonized.
 *
 * @return a new geometry to be freed by the caller with OGR_G_DestroyGeometry,
 * or NULL if an error occurs.
 *
 * @since OGR 1.9.0
 */

OGRGeometryH OGR_G_Polygonize(OGRGeometryH hTarget)

{
    VALIDATE_POINTER1(hTarget, "OGR_G_Polygonize", nullptr);

    return OGRGeometry::ToHandle(
        OGRGeometry::FromHandle(hTarget)->Polygonize());
}

/************************************************************************/
/*                             BuildArea()                              */
/************************************************************************/

/**
 * \brief Polygonize a linework assuming inner polygons are holes.
 *
 * This method is the same as the C function OGR_G_BuildArea().
 *
 * Polygonization is performed similarly to OGRGeometry::Polygonize().
 * Additionally, holes are dropped and the result is unified producing
 * a single Polygon or a MultiPolygon.
 *
 * A new geometry object is created and returned: NULL on failure,
 * empty GeometryCollection if the input geometry cannot be polygonized,
 * Polygon or MultiPolygon on success.
 *
 * This method is built on the GEOSBuildArea_r() function of the GEOS
 * library, check it for the definition of the geometry operation.
 * If OGR is built without the GEOS library, this method will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @return a newly allocated geometry now owned by the caller,
 *         or NULL on failure.
 *
 * @since OGR 3.11
 */

OGRGeometry *OGRGeometry::BuildArea() const

{
#ifndef HAVE_GEOS

    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return nullptr;

#else

    OGRGeometry *poPolygsOGRGeom = nullptr;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    GEOSGeom hThisGeosGeom = exportToGEOS(hGEOSCtxt);
    if (hThisGeosGeom != nullptr)
    {
        GEOSGeom hGeosPolygs = GEOSBuildArea_r(hGEOSCtxt, hThisGeosGeom);
        poPolygsOGRGeom =
            BuildGeometryFromGEOS(hGEOSCtxt, hGeosPolygs, this, nullptr);
        GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);
    }
    freeGEOSContext(hGEOSCtxt);

    return poPolygsOGRGeom;

#endif  // HAVE_GEOS
}

/************************************************************************/
/*                          OGR_G_BuildArea()                           */
/************************************************************************/

/**
 * \brief Polygonize a linework assuming inner polygons are holes.
 *
 * This function is the same as the C++ method OGRGeometry::BuildArea().
 *
 * Polygonization is performed similarly to OGR_G_Polygonize().
 * Additionally, holes are dropped and the result is unified producing
 * a single Polygon or a MultiPolygon.
 *
 * A new geometry object is created and returned: NULL on failure,
 * empty GeometryCollection if the input geometry cannot be polygonized,
 * Polygon or MultiPolygon on success.
 *
 * This function is built on the GEOSBuildArea_r() function of the GEOS
 * library, check it for the definition of the geometry operation.
 * If OGR is built without the GEOS library, this function will always fail,
 * issuing a CPLE_NotSupported error.
 *
 * @param hGeom handle on the geometry to polygonize.
 *
 * @return a handle on newly allocated geometry now owned by the caller,
 *         or NULL on failure.
 *
 * @since OGR 3.11
 */

OGRGeometryH OGR_G_BuildArea(OGRGeometryH hGeom)

{
    VALIDATE_POINTER1(hGeom, "OGR_G_BuildArea", nullptr);

    return OGRGeometry::ToHandle(OGRGeometry::FromHandle(hGeom)->BuildArea());
}

/************************************************************************/
/*                               swapXY()                               */
/************************************************************************/

/**
 * \brief Swap x and y coordinates.
 *
 * @since OGR 1.8.0
 */

void OGRGeometry::swapXY()

{
}

/************************************************************************/
/*                               swapXY()                               */
/************************************************************************/

/**
 * \brief Swap x and y coordinates.
 *
 * @param hGeom geometry.
 * @since OGR 2.3.0
 */

void OGR_G_SwapXY(OGRGeometryH hGeom)
{
    VALIDATE_POINTER0(hGeom, "OGR_G_SwapXY");

    OGRGeometry::FromHandle(hGeom)->swapXY();
}

/************************************************************************/
/*                        Prepared geometry API                         */
/************************************************************************/

#if defined(HAVE_GEOS)
struct _OGRPreparedGeometry
{
    GEOSContextHandle_t hGEOSCtxt;
    GEOSGeom hGEOSGeom;
    const GEOSPreparedGeometry *poPreparedGEOSGeom;
};
#endif

/************************************************************************/
/*                       OGRHasPreparedGeometrySupport()                */
/************************************************************************/

/** Returns if GEOS has prepared geometry support.
 * @return TRUE or FALSE
 */
int OGRHasPreparedGeometrySupport()
{
#if defined(HAVE_GEOS)
    return TRUE;
#else
    return FALSE;
#endif
}

/************************************************************************/
/*                         OGRCreatePreparedGeometry()                  */
/************************************************************************/

/** Creates a prepared geometry.
 *
 * To free with OGRDestroyPreparedGeometry()
 *
 * @param hGeom input geometry to prepare.
 * @return handle to a prepared geometry.
 * @since GDAL 3.3
 */
OGRPreparedGeometryH
OGRCreatePreparedGeometry(UNUSED_IF_NO_GEOS OGRGeometryH hGeom)
{
#if defined(HAVE_GEOS)
    OGRGeometry *poGeom = OGRGeometry::FromHandle(hGeom);
    GEOSContextHandle_t hGEOSCtxt = OGRGeometry::createGEOSContext();
    GEOSGeom hGEOSGeom = poGeom->exportToGEOS(hGEOSCtxt);
    if (hGEOSGeom == nullptr)
    {
        OGRGeometry::freeGEOSContext(hGEOSCtxt);
        return nullptr;
    }
    const GEOSPreparedGeometry *poPreparedGEOSGeom =
        GEOSPrepare_r(hGEOSCtxt, hGEOSGeom);
    if (poPreparedGEOSGeom == nullptr)
    {
        GEOSGeom_destroy_r(hGEOSCtxt, hGEOSGeom);
        OGRGeometry::freeGEOSContext(hGEOSCtxt);
        return nullptr;
    }

    OGRPreparedGeometry *poPreparedGeom = new OGRPreparedGeometry;
    poPreparedGeom->hGEOSCtxt = hGEOSCtxt;
    poPreparedGeom->hGEOSGeom = hGEOSGeom;
    poPreparedGeom->poPreparedGEOSGeom = poPreparedGEOSGeom;

    return poPreparedGeom;
#else
    return nullptr;
#endif
}

/************************************************************************/
/*                        OGRDestroyPreparedGeometry()                  */
/************************************************************************/

/** Destroys a prepared geometry.
 * @param hPreparedGeom prepared geometry.
 * @since GDAL 3.3
 */
void OGRDestroyPreparedGeometry(
    UNUSED_IF_NO_GEOS OGRPreparedGeometryH hPreparedGeom)
{
#if defined(HAVE_GEOS)
    if (hPreparedGeom != nullptr)
    {
        GEOSPreparedGeom_destroy_r(hPreparedGeom->hGEOSCtxt,
                                   hPreparedGeom->poPreparedGEOSGeom);
        GEOSGeom_destroy_r(hPreparedGeom->hGEOSCtxt, hPreparedGeom->hGEOSGeom);
        OGRGeometry::freeGEOSContext(hPreparedGeom->hGEOSCtxt);
        delete hPreparedGeom;
    }
#endif
}

/************************************************************************/
/*                      OGRPreparedGeometryIntersects()                 */
/************************************************************************/

/** Returns whether a prepared geometry intersects with a geometry.
 * @param hPreparedGeom prepared geometry.
 * @param hOtherGeom other geometry.
 * @return TRUE or FALSE.
 * @since GDAL 3.3
 */
int OGRPreparedGeometryIntersects(
    UNUSED_IF_NO_GEOS const OGRPreparedGeometryH hPreparedGeom,
    UNUSED_IF_NO_GEOS const OGRGeometryH hOtherGeom)
{
#if defined(HAVE_GEOS)
    OGRGeometry *poOtherGeom = OGRGeometry::FromHandle(hOtherGeom);
    if (hPreparedGeom == nullptr ||
        poOtherGeom == nullptr
        // The check for IsEmpty() is for buggy GEOS versions.
        // See https://github.com/libgeos/geos/pull/423
        || poOtherGeom->IsEmpty())
    {
        return FALSE;
    }

    GEOSGeom hGEOSOtherGeom =
        poOtherGeom->exportToGEOS(hPreparedGeom->hGEOSCtxt);
    if (hGEOSOtherGeom == nullptr)
        return FALSE;

    const bool bRet = CPL_TO_BOOL(GEOSPreparedIntersects_r(
        hPreparedGeom->hGEOSCtxt, hPreparedGeom->poPreparedGEOSGeom,
        hGEOSOtherGeom));
    GEOSGeom_destroy_r(hPreparedGeom->hGEOSCtxt, hGEOSOtherGeom);

    return bRet;
#else
    return FALSE;
#endif
}

/** Returns whether a prepared geometry contains a geometry.
 * @param hPreparedGeom prepared geometry.
 * @param hOtherGeom other geometry.
 * @return TRUE or FALSE.
 */
int OGRPreparedGeometryContains(UNUSED_IF_NO_GEOS const OGRPreparedGeometryH
                                    hPreparedGeom,
                                UNUSED_IF_NO_GEOS const OGRGeometryH hOtherGeom)
{
#if defined(HAVE_GEOS)
    OGRGeometry *poOtherGeom = OGRGeometry::FromHandle(hOtherGeom);
    if (hPreparedGeom == nullptr ||
        poOtherGeom == nullptr
        // The check for IsEmpty() is for buggy GEOS versions.
        // See https://github.com/libgeos/geos/pull/423
        || poOtherGeom->IsEmpty())
    {
        return FALSE;
    }

    GEOSGeom hGEOSOtherGeom =
        poOtherGeom->exportToGEOS(hPreparedGeom->hGEOSCtxt);
    if (hGEOSOtherGeom == nullptr)
        return FALSE;

    const bool bRet = CPL_TO_BOOL(GEOSPreparedContains_r(
        hPreparedGeom->hGEOSCtxt, hPreparedGeom->poPreparedGEOSGeom,
        hGEOSOtherGeom));
    GEOSGeom_destroy_r(hPreparedGeom->hGEOSCtxt, hGEOSOtherGeom);

    return bRet;
#else
    return FALSE;
#endif
}

/************************************************************************/
/*                       OGRGeometryFromEWKB()                          */
/************************************************************************/

OGRGeometry *OGRGeometryFromEWKB(GByte *pabyEWKB, int nLength, int *pnSRID,
                                 int bIsPostGIS1_EWKB)

{
    OGRGeometry *poGeometry = nullptr;

    size_t nWKBSize = 0;
    const GByte *pabyWKB = WKBFromEWKB(pabyEWKB, nLength, nWKBSize, pnSRID);
    if (pabyWKB == nullptr)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Try to ingest the geometry.                                     */
    /* -------------------------------------------------------------------- */
    (void)OGRGeometryFactory::createFromWkb(
        pabyWKB, nullptr, &poGeometry, nWKBSize,
        (bIsPostGIS1_EWKB) ? wkbVariantPostGIS1 : wkbVariantOldOgc);

    return poGeometry;
}

/************************************************************************/
/*                     OGRGeometryFromHexEWKB()                         */
/************************************************************************/

OGRGeometry *OGRGeometryFromHexEWKB(const char *pszBytea, int *pnSRID,
                                    int bIsPostGIS1_EWKB)

{
    if (pszBytea == nullptr)
        return nullptr;

    int nWKBLength = 0;
    GByte *pabyWKB = CPLHexToBinary(pszBytea, &nWKBLength);

    OGRGeometry *poGeometry =
        OGRGeometryFromEWKB(pabyWKB, nWKBLength, pnSRID, bIsPostGIS1_EWKB);

    CPLFree(pabyWKB);

    return poGeometry;
}

/************************************************************************/
/*                       OGRGeometryToHexEWKB()                         */
/************************************************************************/

char *OGRGeometryToHexEWKB(OGRGeometry *poGeometry, int nSRSId,
                           int nPostGISMajor, int nPostGISMinor)
{
    const size_t nWkbSize = poGeometry->WkbSize();
    GByte *pabyWKB = static_cast<GByte *>(VSI_MALLOC_VERBOSE(nWkbSize));
    if (pabyWKB == nullptr)
        return CPLStrdup("");

    if ((nPostGISMajor > 2 || (nPostGISMajor == 2 && nPostGISMinor >= 2)) &&
        wkbFlatten(poGeometry->getGeometryType()) == wkbPoint &&
        poGeometry->IsEmpty())
    {
        if (poGeometry->exportToWkb(wkbNDR, pabyWKB, wkbVariantIso) !=
            OGRERR_NONE)
        {
            CPLFree(pabyWKB);
            return CPLStrdup("");
        }
    }
    else if (poGeometry->exportToWkb(wkbNDR, pabyWKB,
                                     (nPostGISMajor < 2)
                                         ? wkbVariantPostGIS1
                                         : wkbVariantOldOgc) != OGRERR_NONE)
    {
        CPLFree(pabyWKB);
        return CPLStrdup("");
    }

    // When converting to hex, each byte takes 2 hex characters.  In addition
    // we add in 8 characters to represent the SRID integer in hex, and
    // one for a null terminator.
    // The limit of INT_MAX = 2 GB is a bit artificial, but at time of writing
    // (2024), PostgreSQL by default cannot handle objects larger than 1 GB:
    // https://github.com/postgres/postgres/blob/5d39becf8ba0080c98fee4b63575552f6800b012/src/include/utils/memutils.h#L40
    if (nWkbSize >
        static_cast<size_t>(std::numeric_limits<int>::max() - 8 - 1) / 2)
    {
        CPLFree(pabyWKB);
        return CPLStrdup("");
    }
    const size_t nTextSize = nWkbSize * 2 + 8 + 1;
    char *pszTextBuf = static_cast<char *>(VSI_MALLOC_VERBOSE(nTextSize));
    if (pszTextBuf == nullptr)
    {
        CPLFree(pabyWKB);
        return CPLStrdup("");
    }
    char *pszTextBufCurrent = pszTextBuf;

    // Convert the 1st byte, which is the endianness flag, to hex.
    char *pszHex = CPLBinaryToHex(1, pabyWKB);
    strcpy(pszTextBufCurrent, pszHex);
    CPLFree(pszHex);
    pszTextBufCurrent += 2;

    // Next, get the geom type which is bytes 2 through 5.
    GUInt32 geomType;
    memcpy(&geomType, pabyWKB + 1, 4);

    // Now add the SRID flag if an SRID is provided.
    if (nSRSId > 0)
    {
        // Change the flag to wkbNDR (little) endianness.
        constexpr GUInt32 WKBSRIDFLAG = 0x20000000;
        GUInt32 nGSrsFlag = CPL_LSBWORD32(WKBSRIDFLAG);
        // Apply the flag.
        geomType = geomType | nGSrsFlag;
    }

    // Now write the geom type which is 4 bytes.
    pszHex = CPLBinaryToHex(4, reinterpret_cast<const GByte *>(&geomType));
    strcpy(pszTextBufCurrent, pszHex);
    CPLFree(pszHex);
    pszTextBufCurrent += 8;

    // Now include SRID if provided.
    if (nSRSId > 0)
    {
        // Force the srsid to wkbNDR (little) endianness.
        const GUInt32 nGSRSId = CPL_LSBWORD32(nSRSId);
        pszHex = CPLBinaryToHex(sizeof(nGSRSId),
                                reinterpret_cast<const GByte *>(&nGSRSId));
        strcpy(pszTextBufCurrent, pszHex);
        CPLFree(pszHex);
        pszTextBufCurrent += 8;
    }

    // Copy the rest of the data over - subtract
    // 5 since we already copied 5 bytes above.
    pszHex = CPLBinaryToHex(static_cast<int>(nWkbSize - 5), pabyWKB + 5);
    CPLFree(pabyWKB);
    if (!pszHex || pszHex[0] == 0)
    {
        CPLFree(pszTextBuf);
        return pszHex;
    }
    strcpy(pszTextBufCurrent, pszHex);
    CPLFree(pszHex);

    return pszTextBuf;
}

/************************************************************************/
/*                       importPreambleFromWkb()                       */
/************************************************************************/

//! @cond Doxygen_Suppress
OGRErr OGRGeometry::importPreambleFromWkb(const unsigned char *pabyData,
                                          size_t nSize,
                                          OGRwkbByteOrder &eByteOrder,
                                          OGRwkbVariant eWkbVariant)
{
    if (nSize < 9 && nSize != static_cast<size_t>(-1))
        return OGRERR_NOT_ENOUGH_DATA;

    /* -------------------------------------------------------------------- */
    /*      Get the byte order byte.                                        */
    /* -------------------------------------------------------------------- */
    int nByteOrder = DB2_V72_FIX_BYTE_ORDER(*pabyData);
    if (!(nByteOrder == wkbXDR || nByteOrder == wkbNDR))
        return OGRERR_CORRUPT_DATA;
    eByteOrder = static_cast<OGRwkbByteOrder>(nByteOrder);

    /* -------------------------------------------------------------------- */
    /*      Get the geometry feature type.                                  */
    /* -------------------------------------------------------------------- */
    OGRwkbGeometryType eGeometryType;
    const OGRErr err =
        OGRReadWKBGeometryType(pabyData, eWkbVariant, &eGeometryType);
    if (wkbHasZ(eGeometryType))
        flags |= OGR_G_3D;
    if (wkbHasM(eGeometryType))
        flags |= OGR_G_MEASURED;

    if (err != OGRERR_NONE || eGeometryType != getGeometryType())
        return OGRERR_CORRUPT_DATA;

    return OGRERR_NONE;
}

/************************************************************************/
/*                    importPreambleOfCollectionFromWkb()              */
/*                                                                      */
/*      Utility method for OGRSimpleCurve, OGRCompoundCurve,            */
/*      OGRCurvePolygon and OGRGeometryCollection.                      */
/************************************************************************/

OGRErr OGRGeometry::importPreambleOfCollectionFromWkb(
    const unsigned char *pabyData, size_t &nSize, size_t &nDataOffset,
    OGRwkbByteOrder &eByteOrder, size_t nMinSubGeomSize, int &nGeomCount,
    OGRwkbVariant eWkbVariant)
{
    nGeomCount = 0;

    OGRErr eErr =
        importPreambleFromWkb(pabyData, nSize, eByteOrder, eWkbVariant);
    if (eErr != OGRERR_NONE)
        return eErr;

    /* -------------------------------------------------------------------- */
    /*      Clear existing Geoms.                                           */
    /* -------------------------------------------------------------------- */
    int _flags = flags;  // flags set in importPreambleFromWkb
    empty();             // may reset flags etc.

    // restore
    if (_flags & OGR_G_3D)
        set3D(TRUE);
    if (_flags & OGR_G_MEASURED)
        setMeasured(TRUE);

    /* -------------------------------------------------------------------- */
    /*      Get the sub-geometry count.                                     */
    /* -------------------------------------------------------------------- */
    memcpy(&nGeomCount, pabyData + 5, 4);

    if (OGR_SWAP(eByteOrder))
        nGeomCount = CPL_SWAP32(nGeomCount);

    if (nGeomCount < 0 ||
        static_cast<size_t>(nGeomCount) >
            std::numeric_limits<size_t>::max() / nMinSubGeomSize)
    {
        nGeomCount = 0;
        return OGRERR_CORRUPT_DATA;
    }
    const size_t nBufferMinSize = nGeomCount * nMinSubGeomSize;

    // Each ring has a minimum of nMinSubGeomSize bytes.
    if (nSize != static_cast<size_t>(-1) && nSize - 9 < nBufferMinSize)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Length of input WKB is too small");
        nGeomCount = 0;
        return OGRERR_NOT_ENOUGH_DATA;
    }

    nDataOffset = 9;
    if (nSize != static_cast<size_t>(-1))
    {
        CPLAssert(nSize >= nDataOffset);
        nSize -= nDataOffset;
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                      importCurveCollectionFromWkt()                  */
/*                                                                      */
/*      Utility method for OGRCompoundCurve, OGRCurvePolygon and        */
/*      OGRMultiCurve.                                                  */
/************************************************************************/

OGRErr OGRGeometry::importCurveCollectionFromWkt(
    const char **ppszInput, int bAllowEmptyComponent, int bAllowLineString,
    int bAllowCurve, int bAllowCompoundCurve,
    OGRErr (*pfnAddCurveDirectly)(OGRGeometry *poSelf, OGRCurve *poCurve))

{
    int bHasZ = FALSE;
    int bHasM = FALSE;
    bool bIsEmpty = false;
    OGRErr eErr = importPreambleFromWkt(ppszInput, &bHasZ, &bHasM, &bIsEmpty);
    flags = 0;
    if (eErr != OGRERR_NONE)
        return eErr;
    if (bHasZ)
        flags |= OGR_G_3D;
    if (bHasM)
        flags |= OGR_G_MEASURED;
    if (bIsEmpty)
        return OGRERR_NONE;

    char szToken[OGR_WKT_TOKEN_MAX];
    const char *pszInput = *ppszInput;
    eErr = OGRERR_NONE;

    // Skip first '('.
    pszInput = OGRWktReadToken(pszInput, szToken);

    /* ==================================================================== */
    /*      Read each curve in turn.  Note that we try to reuse the same    */
    /*      point list buffer from curve to curve to cut down on            */
    /*      allocate/deallocate overhead.                                   */
    /* ==================================================================== */
    OGRRawPoint *paoPoints = nullptr;
    int nMaxPoints = 0;
    double *padfZ = nullptr;

    do
    {

        /* --------------------------------------------------------------------
         */
        /*      Get the first token, which should be the geometry type. */
        /* --------------------------------------------------------------------
         */
        const char *pszInputBefore = pszInput;
        pszInput = OGRWktReadToken(pszInput, szToken);

        /* --------------------------------------------------------------------
         */
        /*      Do the import. */
        /* --------------------------------------------------------------------
         */
        OGRCurve *poCurve = nullptr;
        if (EQUAL(szToken, "("))
        {
            OGRLineString *poLine = new OGRLineString();
            poCurve = poLine;
            pszInput = pszInputBefore;
            eErr = poLine->importFromWKTListOnly(&pszInput, bHasZ, bHasM,
                                                 paoPoints, nMaxPoints, padfZ);
        }
        else if (bAllowEmptyComponent && EQUAL(szToken, "EMPTY"))
        {
            poCurve = new OGRLineString();
        }
        // Accept LINESTRING(), but this is an extension to the BNF, also
        // accepted by PostGIS.
        else if ((bAllowLineString && STARTS_WITH_CI(szToken, "LINESTRING")) ||
                 (bAllowCurve && !STARTS_WITH_CI(szToken, "LINESTRING") &&
                  !STARTS_WITH_CI(szToken, "COMPOUNDCURVE") &&
                  OGR_GT_IsCurve(OGRFromOGCGeomType(szToken))) ||
                 (bAllowCompoundCurve &&
                  STARTS_WITH_CI(szToken, "COMPOUNDCURVE")))
        {
            OGRGeometry *poGeom = nullptr;
            pszInput = pszInputBefore;
            eErr =
                OGRGeometryFactory::createFromWkt(&pszInput, nullptr, &poGeom);
            if (poGeom == nullptr)
            {
                eErr = OGRERR_CORRUPT_DATA;
            }
            else
            {
                poCurve = poGeom->toCurve();
            }
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Unexpected token : %s",
                     szToken);
            eErr = OGRERR_CORRUPT_DATA;
        }

        // If this has M it is an error if poGeom does not have M.
        if (poCurve && !Is3D() && IsMeasured() && !poCurve->IsMeasured())
            eErr = OGRERR_CORRUPT_DATA;

        if (eErr == OGRERR_NONE)
            eErr = pfnAddCurveDirectly(this, poCurve);
        if (eErr != OGRERR_NONE)
        {
            delete poCurve;
            break;
        }

        /* --------------------------------------------------------------------
         */
        /*      Read the delimiter following the surface. */
        /* --------------------------------------------------------------------
         */
        pszInput = OGRWktReadToken(pszInput, szToken);
    } while (szToken[0] == ',' && eErr == OGRERR_NONE);

    CPLFree(paoPoints);
    CPLFree(padfZ);

    /* -------------------------------------------------------------------- */
    /*      freak if we don't get a closing bracket.                        */
    /* -------------------------------------------------------------------- */

    if (eErr != OGRERR_NONE)
        return eErr;

    if (szToken[0] != ')')
        return OGRERR_CORRUPT_DATA;

    *ppszInput = pszInput;
    return OGRERR_NONE;
}

//! @endcond

/************************************************************************/
/*                          OGR_GT_Flatten()                            */
/************************************************************************/
/**
 * \brief Returns the 2D geometry type corresponding to the passed geometry
 * type.
 *
 * This function is intended to work with geometry types as old-style 99-402
 * extended dimension (Z) WKB types, as well as with newer SFSQL 1.2 and
 * ISO SQL/MM Part 3 extended dimension (Z&M) WKB types.
 *
 * @param eType Input geometry type
 *
 * @return 2D geometry type corresponding to the passed geometry type.
 *
 * @since GDAL 2.0
 */

OGRwkbGeometryType OGR_GT_Flatten(OGRwkbGeometryType eType)
{
    eType = static_cast<OGRwkbGeometryType>(eType & (~wkb25DBitInternalUse));
    if (eType >= 1000 && eType < 2000)  // ISO Z.
        return static_cast<OGRwkbGeometryType>(eType - 1000);
    if (eType >= 2000 && eType < 3000)  // ISO M.
        return static_cast<OGRwkbGeometryType>(eType - 2000);
    if (eType >= 3000 && eType < 4000)  // ISO ZM.
        return static_cast<OGRwkbGeometryType>(eType - 3000);
    return eType;
}

/************************************************************************/
/*                          OGR_GT_HasZ()                               */
/************************************************************************/
/**
 * \brief Return if the geometry type is a 3D geometry type.
 *
 * @param eType Input geometry type
 *
 * @return TRUE if the geometry type is a 3D geometry type.
 *
 * @since GDAL 2.0
 */

int OGR_GT_HasZ(OGRwkbGeometryType eType)
{
    if (eType & wkb25DBitInternalUse)
        return TRUE;
    if (eType >= 1000 && eType < 2000)  // Accept 1000 for wkbUnknownZ.
        return TRUE;
    if (eType >= 3000 && eType < 4000)  // Accept 3000 for wkbUnknownZM.
        return TRUE;
    return FALSE;
}

/************************************************************************/
/*                          OGR_GT_HasM()                               */
/************************************************************************/
/**
 * \brief Return if the geometry type is a measured type.
 *
 * @param eType Input geometry type
 *
 * @return TRUE if the geometry type is a measured type.
 *
 * @since GDAL 2.1
 */

int OGR_GT_HasM(OGRwkbGeometryType eType)
{
    if (eType >= 2000 && eType < 3000)  // Accept 2000 for wkbUnknownM.
        return TRUE;
    if (eType >= 3000 && eType < 4000)  // Accept 3000 for wkbUnknownZM.
        return TRUE;
    return FALSE;
}

/************************************************************************/
/*                           OGR_GT_SetZ()                              */
/************************************************************************/
/**
 * \brief Returns the 3D geometry type corresponding to the passed geometry
 * type.
 *
 * @param eType Input geometry type
 *
 * @return 3D geometry type corresponding to the passed geometry type.
 *
 * @since GDAL 2.0
 */

OGRwkbGeometryType OGR_GT_SetZ(OGRwkbGeometryType eType)
{
    if (OGR_GT_HasZ(eType) || eType == wkbNone)
        return eType;
    if (eType <= wkbGeometryCollection)
        return static_cast<OGRwkbGeometryType>(eType | wkb25DBitInternalUse);
    else
        return static_cast<OGRwkbGeometryType>(eType + 1000);
}

/************************************************************************/
/*                           OGR_GT_SetM()                              */
/************************************************************************/
/**
 * \brief Returns the measured geometry type corresponding to the passed
 * geometry type.
 *
 * @param eType Input geometry type
 *
 * @return measured geometry type corresponding to the passed geometry type.
 *
 * @since GDAL 2.1
 */

OGRwkbGeometryType OGR_GT_SetM(OGRwkbGeometryType eType)
{
    if (OGR_GT_HasM(eType) || eType == wkbNone)
        return eType;
    if (eType & wkb25DBitInternalUse)
    {
        eType = static_cast<OGRwkbGeometryType>(eType & ~wkb25DBitInternalUse);
        eType = static_cast<OGRwkbGeometryType>(eType + 1000);
    }
    return static_cast<OGRwkbGeometryType>(eType + 2000);
}

/************************************************************************/
/*                        OGR_GT_SetModifier()                          */
/************************************************************************/
/**
 * \brief Returns a XY, XYZ, XYM or XYZM geometry type depending on parameter.
 *
 * @param eType Input geometry type
 * @param bHasZ TRUE if the output geometry type must be 3D.
 * @param bHasM TRUE if the output geometry type must be measured.
 *
 * @return Output geometry type.
 *
 * @since GDAL 2.0
 */

OGRwkbGeometryType OGR_GT_SetModifier(OGRwkbGeometryType eType, int bHasZ,
                                      int bHasM)
{
    if (bHasZ && bHasM)
        return OGR_GT_SetM(OGR_GT_SetZ(eType));
    else if (bHasM)
        return OGR_GT_SetM(wkbFlatten(eType));
    else if (bHasZ)
        return OGR_GT_SetZ(wkbFlatten(eType));
    else
        return wkbFlatten(eType);
}

/************************************************************************/
/*                        OGR_GT_IsSubClassOf)                          */
/************************************************************************/
/**
 * \brief Returns if a type is a subclass of another one
 *
 * @param eType Type.
 * @param eSuperType Super type
 *
 * @return TRUE if eType is a subclass of eSuperType.
 *
 * @since GDAL 2.0
 */

int OGR_GT_IsSubClassOf(OGRwkbGeometryType eType, OGRwkbGeometryType eSuperType)
{
    eSuperType = wkbFlatten(eSuperType);
    eType = wkbFlatten(eType);

    if (eSuperType == eType || eSuperType == wkbUnknown)
        return TRUE;

    if (eSuperType == wkbGeometryCollection)
        return eType == wkbMultiPoint || eType == wkbMultiLineString ||
               eType == wkbMultiPolygon || eType == wkbMultiCurve ||
               eType == wkbMultiSurface;

    if (eSuperType == wkbCurvePolygon)
        return eType == wkbPolygon || eType == wkbTriangle;

    if (eSuperType == wkbMultiCurve)
        return eType == wkbMultiLineString;

    if (eSuperType == wkbMultiSurface)
        return eType == wkbMultiPolygon;

    if (eSuperType == wkbCurve)
        return eType == wkbLineString || eType == wkbCircularString ||
               eType == wkbCompoundCurve;

    if (eSuperType == wkbSurface)
        return eType == wkbCurvePolygon || eType == wkbPolygon ||
               eType == wkbTriangle || eType == wkbPolyhedralSurface ||
               eType == wkbTIN;

    if (eSuperType == wkbPolygon)
        return eType == wkbTriangle;

    if (eSuperType == wkbPolyhedralSurface)
        return eType == wkbTIN;

    return FALSE;
}

/************************************************************************/
/*                       OGR_GT_GetCollection()                         */
/************************************************************************/
/**
 * \brief Returns the collection type that can contain the passed geometry type
 *
 * Handled conversions are : wkbNone->wkbNone, wkbPoint -> wkbMultiPoint,
 * wkbLineString->wkbMultiLineString,
 * wkbPolygon/wkbTriangle/wkbPolyhedralSurface/wkbTIN->wkbMultiPolygon,
 * wkbCircularString->wkbMultiCurve, wkbCompoundCurve->wkbMultiCurve,
 * wkbCurvePolygon->wkbMultiSurface.
 * In other cases, wkbUnknown is returned
 *
 * Passed Z, M, ZM flag is preserved.
 *
 *
 * @param eType Input geometry type
 *
 * @return the collection type that can contain the passed geometry type or
 * wkbUnknown
 *
 * @since GDAL 2.0
 */

OGRwkbGeometryType OGR_GT_GetCollection(OGRwkbGeometryType eType)
{
    const bool bHasZ = wkbHasZ(eType);
    const bool bHasM = wkbHasM(eType);
    if (eType == wkbNone)
        return wkbNone;
    OGRwkbGeometryType eFGType = wkbFlatten(eType);
    if (eFGType == wkbPoint)
        eType = wkbMultiPoint;

    else if (eFGType == wkbLineString)
        eType = wkbMultiLineString;

    else if (eFGType == wkbPolygon)
        eType = wkbMultiPolygon;

    else if (eFGType == wkbTriangle)
        eType = wkbTIN;

    else if (OGR_GT_IsCurve(eFGType))
        eType = wkbMultiCurve;

    else if (OGR_GT_IsSurface(eFGType))
        eType = wkbMultiSurface;

    else
        return wkbUnknown;

    if (bHasZ)
        eType = wkbSetZ(eType);
    if (bHasM)
        eType = wkbSetM(eType);

    return eType;
}

/************************************************************************/
/*                         OGR_GT_GetSingle()                           */
/************************************************************************/
/**
 * \brief Returns the non-collection type that be contained in the passed
 * geometry type.
 *
 * Handled conversions are : wkbNone->wkbNone, wkbMultiPoint -> wkbPoint,
 * wkbMultiLineString -> wkbLineString, wkbMultiPolygon -> wkbPolygon,
 * wkbMultiCurve -> wkbCompoundCurve, wkbMultiSurface -> wkbCurvePolygon,
 * wkbGeometryCollection -> wkbUnknown
 * In other cases, the original geometry is returned.
 *
 * Passed Z, M, ZM flag is preserved.
 *
 *
 * @param eType Input geometry type
 *
 * @return the the non-collection type that be contained in the passed geometry
 * type or wkbUnknown
 *
 * @since GDAL 3.11
 */

OGRwkbGeometryType OGR_GT_GetSingle(OGRwkbGeometryType eType)
{
    const bool bHasZ = wkbHasZ(eType);
    const bool bHasM = wkbHasM(eType);
    if (eType == wkbNone)
        return wkbNone;
    const OGRwkbGeometryType eFGType = wkbFlatten(eType);
    if (eFGType == wkbMultiPoint)
        eType = wkbPoint;

    else if (eFGType == wkbMultiLineString)
        eType = wkbLineString;

    else if (eFGType == wkbMultiPolygon)
        eType = wkbPolygon;

    else if (eFGType == wkbMultiCurve)
        eType = wkbCompoundCurve;

    else if (eFGType == wkbMultiSurface)
        eType = wkbCurvePolygon;

    else if (eFGType == wkbGeometryCollection)
        return wkbUnknown;

    if (bHasZ)
        eType = wkbSetZ(eType);
    if (bHasM)
        eType = wkbSetM(eType);

    return eType;
}

/************************************************************************/
/*                        OGR_GT_GetCurve()                             */
/************************************************************************/
/**
 * \brief Returns the curve geometry type that can contain the passed geometry
 * type
 *
 * Handled conversions are : wkbPolygon -> wkbCurvePolygon,
 * wkbLineString->wkbCompoundCurve, wkbMultiPolygon->wkbMultiSurface
 * and wkbMultiLineString->wkbMultiCurve.
 * In other cases, the passed geometry is returned.
 *
 * Passed Z, M, ZM flag is preserved.
 *
 * @param eType Input geometry type
 *
 * @return the curve type that can contain the passed geometry type
 *
 * @since GDAL 2.0
 */

OGRwkbGeometryType OGR_GT_GetCurve(OGRwkbGeometryType eType)
{
    const bool bHasZ = wkbHasZ(eType);
    const bool bHasM = wkbHasM(eType);
    OGRwkbGeometryType eFGType = wkbFlatten(eType);

    if (eFGType == wkbLineString)
        eType = wkbCompoundCurve;

    else if (eFGType == wkbPolygon)
        eType = wkbCurvePolygon;

    else if (eFGType == wkbTriangle)
        eType = wkbCurvePolygon;

    else if (eFGType == wkbMultiLineString)
        eType = wkbMultiCurve;

    else if (eFGType == wkbMultiPolygon)
        eType = wkbMultiSurface;

    if (bHasZ)
        eType = wkbSetZ(eType);
    if (bHasM)
        eType = wkbSetM(eType);

    return eType;
}

/************************************************************************/
/*                        OGR_GT_GetLinear()                          */
/************************************************************************/
/**
 * \brief Returns the non-curve geometry type that can contain the passed
 * geometry type
 *
 * Handled conversions are : wkbCurvePolygon -> wkbPolygon,
 * wkbCircularString->wkbLineString, wkbCompoundCurve->wkbLineString,
 * wkbMultiSurface->wkbMultiPolygon and wkbMultiCurve->wkbMultiLineString.
 * In other cases, the passed geometry is returned.
 *
 * Passed Z, M, ZM flag is preserved.
 *
 * @param eType Input geometry type
 *
 * @return the non-curve type that can contain the passed geometry type
 *
 * @since GDAL 2.0
 */

OGRwkbGeometryType OGR_GT_GetLinear(OGRwkbGeometryType eType)
{
    const bool bHasZ = wkbHasZ(eType);
    const bool bHasM = wkbHasM(eType);
    OGRwkbGeometryType eFGType = wkbFlatten(eType);

    if (OGR_GT_IsCurve(eFGType))
        eType = wkbLineString;

    else if (OGR_GT_IsSurface(eFGType))
        eType = wkbPolygon;

    else if (eFGType == wkbMultiCurve)
        eType = wkbMultiLineString;

    else if (eFGType == wkbMultiSurface)
        eType = wkbMultiPolygon;

    if (bHasZ)
        eType = wkbSetZ(eType);
    if (bHasM)
        eType = wkbSetM(eType);

    return eType;
}

/************************************************************************/
/*                           OGR_GT_IsCurve()                           */
/************************************************************************/

/**
 * \brief Return if a geometry type is an instance of Curve
 *
 * Such geometry type are wkbLineString, wkbCircularString, wkbCompoundCurve
 * and their Z/M/ZM variant.
 *
 * @param eGeomType the geometry type
 * @return TRUE if the geometry type is an instance of Curve
 *
 * @since GDAL 2.0
 */

int OGR_GT_IsCurve(OGRwkbGeometryType eGeomType)
{
    return OGR_GT_IsSubClassOf(eGeomType, wkbCurve);
}

/************************************************************************/
/*                         OGR_GT_IsSurface()                           */
/************************************************************************/

/**
 * \brief Return if a geometry type is an instance of Surface
 *
 * Such geometry type are wkbCurvePolygon and wkbPolygon
 * and their Z/M/ZM variant.
 *
 * @param eGeomType the geometry type
 * @return TRUE if the geometry type is an instance of Surface
 *
 * @since GDAL 2.0
 */

int OGR_GT_IsSurface(OGRwkbGeometryType eGeomType)
{
    return OGR_GT_IsSubClassOf(eGeomType, wkbSurface);
}

/************************************************************************/
/*                          OGR_GT_IsNonLinear()                        */
/************************************************************************/

/**
 * \brief Return if a geometry type is a non-linear geometry type.
 *
 * Such geometry type are wkbCurve, wkbCircularString, wkbCompoundCurve,
 * wkbSurface, wkbCurvePolygon, wkbMultiCurve, wkbMultiSurface and their
 * Z/M variants.
 *
 * @param eGeomType the geometry type
 * @return TRUE if the geometry type is a non-linear geometry type.
 *
 * @since GDAL 2.0
 */

int OGR_GT_IsNonLinear(OGRwkbGeometryType eGeomType)
{
    OGRwkbGeometryType eFGeomType = wkbFlatten(eGeomType);
    return eFGeomType == wkbCurve || eFGeomType == wkbSurface ||
           eFGeomType == wkbCircularString || eFGeomType == wkbCompoundCurve ||
           eFGeomType == wkbCurvePolygon || eFGeomType == wkbMultiCurve ||
           eFGeomType == wkbMultiSurface;
}

/************************************************************************/
/*                          CastToError()                               */
/************************************************************************/

//! @cond Doxygen_Suppress
OGRGeometry *OGRGeometry::CastToError(OGRGeometry *poGeom)
{
    CPLError(CE_Failure, CPLE_AppDefined, "%s found. Conversion impossible",
             poGeom->getGeometryName());
    delete poGeom;
    return nullptr;
}

//! @endcond

/************************************************************************/
/*                          OGRexportToSFCGAL()                         */
/************************************************************************/

//! @cond Doxygen_Suppress
sfcgal_geometry_t *
OGRGeometry::OGRexportToSFCGAL(UNUSED_IF_NO_SFCGAL const OGRGeometry *poGeom)
{
#ifdef HAVE_SFCGAL

    sfcgal_init();
#if SFCGAL_VERSION >= SFCGAL_MAKE_VERSION(1, 5, 2)

    const auto exportToSFCGALViaWKB =
        [](const OGRGeometry *geom) -> sfcgal_geometry_t *
    {
        if (!geom)
            return nullptr;

        // Get WKB size and allocate buffer
        size_t nSize = geom->WkbSize();
        unsigned char *pabyWkb = static_cast<unsigned char *>(CPLMalloc(nSize));

        // Set export options with NDR byte order
        OGRwkbExportOptions oOptions;
        oOptions.eByteOrder = wkbNDR;
        // and ISO to avoid wkb25DBit for Z geometries
        oOptions.eWkbVariant = wkbVariantIso;

        // Export to WKB
        sfcgal_geometry_t *sfcgalGeom = nullptr;
        if (geom->exportToWkb(pabyWkb, &oOptions) == OGRERR_NONE)
        {
            sfcgalGeom = sfcgal_io_read_wkb(
                reinterpret_cast<const char *>(pabyWkb), nSize);
        }

        CPLFree(pabyWkb);
        return sfcgalGeom;
    };

    // Handle special cases
    if (EQUAL(poGeom->getGeometryName(), "LINEARRING"))
    {
        std::unique_ptr<OGRLineString> poLS(
            OGRCurve::CastToLineString(poGeom->clone()->toCurve()));
        return exportToSFCGALViaWKB(poLS.get());
    }
    else if (EQUAL(poGeom->getGeometryName(), "CIRCULARSTRING") ||
             EQUAL(poGeom->getGeometryName(), "COMPOUNDCURVE"))
    {
        std::unique_ptr<OGRLineString> poLS(
            OGRGeometryFactory::forceToLineString(poGeom->clone())
                ->toLineString());
        return exportToSFCGALViaWKB(poLS.get());
    }
    else if (EQUAL(poGeom->getGeometryName(), "CURVEPOLYGON"))
    {
        std::unique_ptr<OGRPolygon> poPolygon(
            OGRGeometryFactory::forceToPolygon(
                poGeom->clone()->toCurvePolygon())
                ->toPolygon());
        return exportToSFCGALViaWKB(poPolygon.get());
    }
    else
    {
        // Default case - direct export
        return exportToSFCGALViaWKB(poGeom);
    }
#else
    char *buffer = nullptr;

    // special cases - LinearRing, Circular String, Compound Curve, Curve
    // Polygon

    if (EQUAL(poGeom->getGeometryName(), "LINEARRING"))
    {
        // cast it to LineString and get the WKT
        std::unique_ptr<OGRLineString> poLS(
            OGRCurve::CastToLineString(poGeom->clone()->toCurve()));
        if (poLS->exportToWkt(&buffer) == OGRERR_NONE)
        {
            sfcgal_geometry_t *_geometry =
                sfcgal_io_read_wkt(buffer, strlen(buffer));
            CPLFree(buffer);
            return _geometry;
        }
        else
        {
            CPLFree(buffer);
            return nullptr;
        }
    }
    else if (EQUAL(poGeom->getGeometryName(), "CIRCULARSTRING") ||
             EQUAL(poGeom->getGeometryName(), "COMPOUNDCURVE"))
    {
        // convert it to LineString and get the WKT
        std::unique_ptr<OGRLineString> poLS(
            OGRGeometryFactory::forceToLineString(poGeom->clone())
                ->toLineString());
        if (poLS->exportToWkt(&buffer) == OGRERR_NONE)
        {
            sfcgal_geometry_t *_geometry =
                sfcgal_io_read_wkt(buffer, strlen(buffer));
            CPLFree(buffer);
            return _geometry;
        }
        else
        {
            CPLFree(buffer);
            return nullptr;
        }
    }
    else if (EQUAL(poGeom->getGeometryName(), "CURVEPOLYGON"))
    {
        // convert it to Polygon and get the WKT
        std::unique_ptr<OGRPolygon> poPolygon(
            OGRGeometryFactory::forceToPolygon(
                poGeom->clone()->toCurvePolygon())
                ->toPolygon());
        if (poPolygon->exportToWkt(&buffer) == OGRERR_NONE)
        {
            sfcgal_geometry_t *_geometry =
                sfcgal_io_read_wkt(buffer, strlen(buffer));
            CPLFree(buffer);
            return _geometry;
        }
        else
        {
            CPLFree(buffer);
            return nullptr;
        }
    }
    else if (poGeom->exportToWkt(&buffer) == OGRERR_NONE)
    {
        sfcgal_geometry_t *_geometry =
            sfcgal_io_read_wkt(buffer, strlen(buffer));
        CPLFree(buffer);
        return _geometry;
    }
    else
    {
        CPLFree(buffer);
        return nullptr;
    }
#endif
#else
    CPLError(CE_Failure, CPLE_NotSupported, "SFCGAL support not enabled.");
    return nullptr;
#endif
}

//! @endcond

/************************************************************************/
/*                          SFCGALexportToOGR()                         */
/************************************************************************/

//! @cond Doxygen_Suppress
OGRGeometry *OGRGeometry::SFCGALexportToOGR(
    UNUSED_IF_NO_SFCGAL const sfcgal_geometry_t *geometry)
{
#ifdef HAVE_SFCGAL
    if (geometry == nullptr)
        return nullptr;

    sfcgal_init();
    char *pabySFCGAL = nullptr;
    size_t nLength = 0;
#if SFCGAL_VERSION >= SFCGAL_MAKE_VERSION(1, 5, 2)

    sfcgal_geometry_as_wkb(geometry, &pabySFCGAL, &nLength);

    if (pabySFCGAL == nullptr || nLength == 0)
        return nullptr;

    OGRGeometry *poGeom = nullptr;
    OGRErr eErr = OGRGeometryFactory::createFromWkb(
        reinterpret_cast<unsigned char *>(pabySFCGAL), nullptr, &poGeom,
        nLength);

    free(pabySFCGAL);

    if (eErr == OGRERR_NONE)
    {
        return poGeom;
    }
    else
    {
        return nullptr;
    }
#else
    sfcgal_geometry_as_text_decim(geometry, 19, &pabySFCGAL, &nLength);
    char *pszWKT = static_cast<char *>(CPLMalloc(nLength + 1));
    memcpy(pszWKT, pabySFCGAL, nLength);
    pszWKT[nLength] = 0;
    free(pabySFCGAL);

    sfcgal_geometry_type_t geom_type = sfcgal_geometry_type_id(geometry);

    OGRGeometry *poGeom = nullptr;
    if (geom_type == SFCGAL_TYPE_POINT)
    {
        poGeom = new OGRPoint();
    }
    else if (geom_type == SFCGAL_TYPE_LINESTRING)
    {
        poGeom = new OGRLineString();
    }
    else if (geom_type == SFCGAL_TYPE_POLYGON)
    {
        poGeom = new OGRPolygon();
    }
    else if (geom_type == SFCGAL_TYPE_MULTIPOINT)
    {
        poGeom = new OGRMultiPoint();
    }
    else if (geom_type == SFCGAL_TYPE_MULTILINESTRING)
    {
        poGeom = new OGRMultiLineString();
    }
    else if (geom_type == SFCGAL_TYPE_MULTIPOLYGON)
    {
        poGeom = new OGRMultiPolygon();
    }
    else if (geom_type == SFCGAL_TYPE_GEOMETRYCOLLECTION)
    {
        poGeom = new OGRGeometryCollection();
    }
    else if (geom_type == SFCGAL_TYPE_TRIANGLE)
    {
        poGeom = new OGRTriangle();
    }
    else if (geom_type == SFCGAL_TYPE_POLYHEDRALSURFACE)
    {
        poGeom = new OGRPolyhedralSurface();
    }
    else if (geom_type == SFCGAL_TYPE_TRIANGULATEDSURFACE)
    {
        poGeom = new OGRTriangulatedSurface();
    }
    else
    {
        CPLFree(pszWKT);
        return nullptr;
    }

    const char *pszWKTTmp = pszWKT;
    if (poGeom->importFromWkt(&pszWKTTmp) == OGRERR_NONE)
    {
        CPLFree(pszWKT);
        return poGeom;
    }
    else
    {
        delete poGeom;
        CPLFree(pszWKT);
        return nullptr;
    }
#endif
#else
    CPLError(CE_Failure, CPLE_NotSupported, "SFCGAL support not enabled.");
    return nullptr;
#endif
}

//! @endcond

//! @cond Doxygen_Suppress
OGRBoolean OGRGeometry::IsSFCGALCompatible() const
{
    const OGRwkbGeometryType eGType = wkbFlatten(getGeometryType());
    if (eGType == wkbTriangle || eGType == wkbPolyhedralSurface ||
        eGType == wkbTIN)
    {
        return TRUE;
    }
    if (eGType == wkbGeometryCollection || eGType == wkbMultiSurface)
    {
        const OGRGeometryCollection *poGC = toGeometryCollection();
        bool bIsSFCGALCompatible = false;
        for (auto &&poSubGeom : *poGC)
        {
            OGRwkbGeometryType eSubGeomType =
                wkbFlatten(poSubGeom->getGeometryType());
            if (eSubGeomType == wkbTIN || eSubGeomType == wkbPolyhedralSurface)
            {
                bIsSFCGALCompatible = true;
            }
            else if (eSubGeomType != wkbMultiPolygon)
            {
                bIsSFCGALCompatible = false;
                break;
            }
        }
        return bIsSFCGALCompatible;
    }
    return FALSE;
}

//! @endcond

/************************************************************************/
/*                    roundCoordinatesIEEE754()                         */
/************************************************************************/

/** Round coordinates of a geometry, exploiting characteristics of the IEEE-754
 * double-precision binary representation.
 *
 * Determines the number of bits (N) required to represent a coordinate value
 * with a specified number of digits after the decimal point, and then sets all
 * but the N most significant bits to zero. The resulting coordinate value will
 * still round to the original value (e.g. after roundCoordinates()), but will
 * have improved compressiblity.
 *
 * @param options Contains the precision requirements.
 * @since GDAL 3.9
 */
void OGRGeometry::roundCoordinatesIEEE754(
    const OGRGeomCoordinateBinaryPrecision &options)
{
    struct Quantizer : public OGRDefaultGeometryVisitor
    {
        const OGRGeomCoordinateBinaryPrecision &m_options;

        explicit Quantizer(const OGRGeomCoordinateBinaryPrecision &optionsIn)
            : m_options(optionsIn)
        {
        }

        using OGRDefaultGeometryVisitor::visit;

        void visit(OGRPoint *poPoint) override
        {
            if (m_options.nXYBitPrecision != INT_MIN)
            {
                uint64_t i;
                double d;
                d = poPoint->getX();
                memcpy(&i, &d, sizeof(i));
                i = OGRRoundValueIEEE754(i, m_options.nXYBitPrecision);
                memcpy(&d, &i, sizeof(i));
                poPoint->setX(d);
                d = poPoint->getY();
                memcpy(&i, &d, sizeof(i));
                i = OGRRoundValueIEEE754(i, m_options.nXYBitPrecision);
                memcpy(&d, &i, sizeof(i));
                poPoint->setY(d);
            }
            if (m_options.nZBitPrecision != INT_MIN && poPoint->Is3D())
            {
                uint64_t i;
                double d;
                d = poPoint->getZ();
                memcpy(&i, &d, sizeof(i));
                i = OGRRoundValueIEEE754(i, m_options.nZBitPrecision);
                memcpy(&d, &i, sizeof(i));
                poPoint->setZ(d);
            }
            if (m_options.nMBitPrecision != INT_MIN && poPoint->IsMeasured())
            {
                uint64_t i;
                double d;
                d = poPoint->getM();
                memcpy(&i, &d, sizeof(i));
                i = OGRRoundValueIEEE754(i, m_options.nMBitPrecision);
                memcpy(&d, &i, sizeof(i));
                poPoint->setM(d);
            }
        }
    };

    Quantizer quantizer(options);
    accept(&quantizer);
}

/************************************************************************/
/*                             visit()                                  */
/************************************************************************/

void OGRDefaultGeometryVisitor::_visit(OGRSimpleCurve *poGeom)
{
    for (auto &&oPoint : *poGeom)
    {
        oPoint.accept(this);
    }
}

void OGRDefaultGeometryVisitor::visit(OGRLineString *poGeom)
{
    _visit(poGeom);
}

void OGRDefaultGeometryVisitor::visit(OGRLinearRing *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultGeometryVisitor::visit(OGRCircularString *poGeom)
{
    _visit(poGeom);
}

void OGRDefaultGeometryVisitor::visit(OGRCurvePolygon *poGeom)
{
    for (auto &&poSubGeom : *poGeom)
        poSubGeom->accept(this);
}

void OGRDefaultGeometryVisitor::visit(OGRPolygon *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultGeometryVisitor::visit(OGRMultiPoint *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultGeometryVisitor::visit(OGRMultiLineString *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultGeometryVisitor::visit(OGRMultiPolygon *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultGeometryVisitor::visit(OGRGeometryCollection *poGeom)
{
    for (auto &&poSubGeom : *poGeom)
        poSubGeom->accept(this);
}

void OGRDefaultGeometryVisitor::visit(OGRCompoundCurve *poGeom)
{
    for (auto &&poSubGeom : *poGeom)
        poSubGeom->accept(this);
}

void OGRDefaultGeometryVisitor::visit(OGRMultiCurve *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultGeometryVisitor::visit(OGRMultiSurface *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultGeometryVisitor::visit(OGRTriangle *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultGeometryVisitor::visit(OGRPolyhedralSurface *poGeom)
{
    for (auto &&poSubGeom : *poGeom)
        poSubGeom->accept(this);
}

void OGRDefaultGeometryVisitor::visit(OGRTriangulatedSurface *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultConstGeometryVisitor::_visit(const OGRSimpleCurve *poGeom)
{
    for (auto &&oPoint : *poGeom)
    {
        oPoint.accept(this);
    }
}

void OGRDefaultConstGeometryVisitor::visit(const OGRLineString *poGeom)
{
    _visit(poGeom);
}

void OGRDefaultConstGeometryVisitor::visit(const OGRLinearRing *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultConstGeometryVisitor::visit(const OGRCircularString *poGeom)
{
    _visit(poGeom);
}

void OGRDefaultConstGeometryVisitor::visit(const OGRCurvePolygon *poGeom)
{
    for (auto &&poSubGeom : *poGeom)
        poSubGeom->accept(this);
}

void OGRDefaultConstGeometryVisitor::visit(const OGRPolygon *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultConstGeometryVisitor::visit(const OGRMultiPoint *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultConstGeometryVisitor::visit(const OGRMultiLineString *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultConstGeometryVisitor::visit(const OGRMultiPolygon *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultConstGeometryVisitor::visit(const OGRGeometryCollection *poGeom)
{
    for (auto &&poSubGeom : *poGeom)
        poSubGeom->accept(this);
}

void OGRDefaultConstGeometryVisitor::visit(const OGRCompoundCurve *poGeom)
{
    for (auto &&poSubGeom : *poGeom)
        poSubGeom->accept(this);
}

void OGRDefaultConstGeometryVisitor::visit(const OGRMultiCurve *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultConstGeometryVisitor::visit(const OGRMultiSurface *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultConstGeometryVisitor::visit(const OGRTriangle *poGeom)
{
    visit(poGeom->toUpperClass());
}

void OGRDefaultConstGeometryVisitor::visit(const OGRPolyhedralSurface *poGeom)
{
    for (auto &&poSubGeom : *poGeom)
        poSubGeom->accept(this);
}

void OGRDefaultConstGeometryVisitor::visit(const OGRTriangulatedSurface *poGeom)
{
    visit(poGeom->toUpperClass());
}

/************************************************************************/
/*                     OGRGeometryUniquePtrDeleter                      */
/************************************************************************/

//! @cond Doxygen_Suppress
void OGRGeometryUniquePtrDeleter::operator()(OGRGeometry *poGeom) const
{
    delete poGeom;
}

//! @endcond

/************************************************************************/
/*                  OGRPreparedGeometryUniquePtrDeleter                 */
/************************************************************************/

//! @cond Doxygen_Suppress
void OGRPreparedGeometryUniquePtrDeleter::operator()(
    OGRPreparedGeometry *poPreparedGeom) const
{
    OGRDestroyPreparedGeometry(poPreparedGeom);
}

//! @endcond

/************************************************************************/
/*                     HomogenizeDimensionalityWith()                  */
/************************************************************************/

//! @cond Doxygen_Suppress
void OGRGeometry::HomogenizeDimensionalityWith(OGRGeometry *poOtherGeom)
{
    if (poOtherGeom->Is3D() && !Is3D())
        set3D(TRUE);

    if (poOtherGeom->IsMeasured() && !IsMeasured())
        setMeasured(TRUE);

    if (!poOtherGeom->Is3D() && Is3D())
        poOtherGeom->set3D(TRUE);

    if (!poOtherGeom->IsMeasured() && IsMeasured())
        poOtherGeom->setMeasured(TRUE);
}

//! @endcond

/************************************************************************/
/*                  OGRGeomCoordinateBinaryPrecision::SetFrom()         */
/************************************************************************/

/** Set binary precision options from resolution.
 *
 * @since GDAL 3.9
 */
void OGRGeomCoordinateBinaryPrecision::SetFrom(
    const OGRGeomCoordinatePrecision &prec)
{
    if (prec.dfXYResolution != 0)
    {
        nXYBitPrecision =
            static_cast<int>(ceil(log2(1. / prec.dfXYResolution)));
    }
    if (prec.dfZResolution != 0)
    {
        nZBitPrecision = static_cast<int>(ceil(log2(1. / prec.dfZResolution)));
    }
    if (prec.dfMResolution != 0)
    {
        nMBitPrecision = static_cast<int>(ceil(log2(1. / prec.dfMResolution)));
    }
}

/************************************************************************/
/*                        OGRwkbExportOptionsCreate()                   */
/************************************************************************/

/**
 * \brief Create geometry WKB export options.
 *
 * The default is Intel order, old-OGC wkb variant and 0 discarded lsb bits.
 *
 * @return object to be freed with OGRwkbExportOptionsDestroy().
 * @since GDAL 3.9
 */
OGRwkbExportOptions *OGRwkbExportOptionsCreate()
{
    return new OGRwkbExportOptions;
}

/************************************************************************/
/*                        OGRwkbExportOptionsDestroy()                  */
/************************************************************************/

/**
 * \brief Destroy object returned by OGRwkbExportOptionsCreate()
 *
 * @param psOptions WKB export options
 * @since GDAL 3.9
 */

void OGRwkbExportOptionsDestroy(OGRwkbExportOptions *psOptions)
{
    delete psOptions;
}

/************************************************************************/
/*                   OGRwkbExportOptionsSetByteOrder()                  */
/************************************************************************/

/**
 * \brief Set the WKB byte order.
 *
 * @param psOptions WKB export options
 * @param eByteOrder Byte order: wkbXDR (big-endian) or wkbNDR (little-endian,
 * Intel)
 * @since GDAL 3.9
 */

void OGRwkbExportOptionsSetByteOrder(OGRwkbExportOptions *psOptions,
                                     OGRwkbByteOrder eByteOrder)
{
    psOptions->eByteOrder = eByteOrder;
}

/************************************************************************/
/*                   OGRwkbExportOptionsSetVariant()                    */
/************************************************************************/

/**
 * \brief Set the WKB variant
 *
 * @param psOptions WKB export options
 * @param eWkbVariant variant: wkbVariantOldOgc, wkbVariantIso,
 * wkbVariantPostGIS1
 * @since GDAL 3.9
 */

void OGRwkbExportOptionsSetVariant(OGRwkbExportOptions *psOptions,
                                   OGRwkbVariant eWkbVariant)
{
    psOptions->eWkbVariant = eWkbVariant;
}

/************************************************************************/
/*                   OGRwkbExportOptionsSetPrecision()                  */
/************************************************************************/

/**
 * \brief Set precision options
 *
 * @param psOptions WKB export options
 * @param hPrecisionOptions Precision options (might be null to reset them)
 * @since GDAL 3.9
 */

void OGRwkbExportOptionsSetPrecision(
    OGRwkbExportOptions *psOptions,
    OGRGeomCoordinatePrecisionH hPrecisionOptions)
{
    psOptions->sPrecision = OGRGeomCoordinateBinaryPrecision();
    if (hPrecisionOptions)
        psOptions->sPrecision.SetFrom(*hPrecisionOptions);
}

/************************************************************************/
/*                             IsRectangle()                            */
/************************************************************************/

/**
 * \brief Returns whether the geometry is a polygon with 4 corners forming
 * a rectangle.
 *
 * @since GDAL 3.10
 */
bool OGRGeometry::IsRectangle() const
{
    if (wkbFlatten(getGeometryType()) != wkbPolygon)
        return false;

    const OGRPolygon *poPoly = toPolygon();

    if (poPoly->getNumInteriorRings() != 0)
        return false;

    const OGRLinearRing *poRing = poPoly->getExteriorRing();
    if (!poRing)
        return false;

    if (poRing->getNumPoints() > 5 || poRing->getNumPoints() < 4)
        return false;

    // If the ring has 5 points, the last should be the first.
    if (poRing->getNumPoints() == 5 && (poRing->getX(0) != poRing->getX(4) ||
                                        poRing->getY(0) != poRing->getY(4)))
        return false;

    // Polygon with first segment in "y" direction.
    if (poRing->getX(0) == poRing->getX(1) &&
        poRing->getY(1) == poRing->getY(2) &&
        poRing->getX(2) == poRing->getX(3) &&
        poRing->getY(3) == poRing->getY(0))
        return true;

    // Polygon with first segment in "x" direction.
    if (poRing->getY(0) == poRing->getY(1) &&
        poRing->getX(1) == poRing->getX(2) &&
        poRing->getY(2) == poRing->getY(3) &&
        poRing->getX(3) == poRing->getX(0))
        return true;

    return false;
}

/************************************************************************/
/*                           hasEmptyParts()                            */
/************************************************************************/

/**
 * \brief Returns whether a geometry has empty parts/rings.
 *
 * Returns true if removeEmptyParts() will modify the geometry.
 *
 * This is different from IsEmpty().
 *
 * @since GDAL 3.10
 */
bool OGRGeometry::hasEmptyParts() const
{
    return false;
}

/************************************************************************/
/*                          removeEmptyParts()                          */
/************************************************************************/

/**
 * \brief Remove empty parts/rings from this geometry.
 *
 * @since GDAL 3.10
 */
void OGRGeometry::removeEmptyParts()
{
}
