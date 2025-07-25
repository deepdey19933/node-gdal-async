/******************************************************************************
 *
 * Project:  Memory Array Translator
 * Purpose:  Complete implementation.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2000, Frank Warmerdam
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "memdataset.h"
#include "memmultidim.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "cpl_config.h"
#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_minixml.h"
#include "cpl_progress.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "gdal.h"
#include "gdal_frmts.h"

struct MEMDataset::Private
{
    std::shared_ptr<GDALGroup> m_poRootGroup{};
};

/************************************************************************/
/*                        MEMCreateRasterBand()                         */
/************************************************************************/

GDALRasterBandH MEMCreateRasterBand(GDALDataset *poDS, int nBand,
                                    GByte *pabyData, GDALDataType eType,
                                    int nPixelOffset, int nLineOffset,
                                    int bAssumeOwnership)

{
    return GDALRasterBand::ToHandle(
        new MEMRasterBand(poDS, nBand, pabyData, eType, nPixelOffset,
                          nLineOffset, bAssumeOwnership));
}

/************************************************************************/
/*                       MEMCreateRasterBandEx()                        */
/************************************************************************/

GDALRasterBandH MEMCreateRasterBandEx(GDALDataset *poDS, int nBand,
                                      GByte *pabyData, GDALDataType eType,
                                      GSpacing nPixelOffset,
                                      GSpacing nLineOffset,
                                      int bAssumeOwnership)

{
    return GDALRasterBand::ToHandle(
        new MEMRasterBand(poDS, nBand, pabyData, eType, nPixelOffset,
                          nLineOffset, bAssumeOwnership));
}

/************************************************************************/
/*                           MEMRasterBand()                            */
/************************************************************************/

MEMRasterBand::MEMRasterBand(GByte *pabyDataIn, GDALDataType eTypeIn,
                             int nXSizeIn, int nYSizeIn, bool bOwnDataIn)
    : GDALPamRasterBand(FALSE), pabyData(pabyDataIn),
      nPixelOffset(GDALGetDataTypeSizeBytes(eTypeIn)), nLineOffset(0),
      bOwnData(bOwnDataIn)
{
    eAccess = GA_Update;
    eDataType = eTypeIn;
    nRasterXSize = nXSizeIn;
    nRasterYSize = nYSizeIn;
    nBlockXSize = nXSizeIn;
    nBlockYSize = 1;
    nLineOffset = nPixelOffset * static_cast<size_t>(nBlockXSize);

    PamInitializeNoParent();
}

/************************************************************************/
/*                           MEMRasterBand()                            */
/************************************************************************/

MEMRasterBand::MEMRasterBand(GDALDataset *poDSIn, int nBandIn,
                             GByte *pabyDataIn, GDALDataType eTypeIn,
                             GSpacing nPixelOffsetIn, GSpacing nLineOffsetIn,
                             int bAssumeOwnership, const char *pszPixelType)
    : GDALPamRasterBand(FALSE), pabyData(pabyDataIn),
      nPixelOffset(nPixelOffsetIn), nLineOffset(nLineOffsetIn),
      bOwnData(bAssumeOwnership)
{
    poDS = poDSIn;
    nBand = nBandIn;

    eAccess = poDS->GetAccess();

    eDataType = eTypeIn;

    nBlockXSize = poDS->GetRasterXSize();
    nBlockYSize = 1;

    if (nPixelOffsetIn == 0)
        nPixelOffset = GDALGetDataTypeSizeBytes(eTypeIn);

    if (nLineOffsetIn == 0)
        nLineOffset = nPixelOffset * static_cast<size_t>(nBlockXSize);

    if (pszPixelType && EQUAL(pszPixelType, "SIGNEDBYTE"))
        SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");

    PamInitializeNoParent();
}

/************************************************************************/
/*                           ~MEMRasterBand()                           */
/************************************************************************/

MEMRasterBand::~MEMRasterBand()

{
    if (bOwnData)
    {
        VSIFree(pabyData);
    }
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr MEMRasterBand::IReadBlock(CPL_UNUSED int nBlockXOff, int nBlockYOff,
                                 void *pImage)
{
    CPLAssert(nBlockXOff == 0);

    const int nWordSize = GDALGetDataTypeSize(eDataType) / 8;

    if (nPixelOffset == nWordSize)
    {
        memcpy(pImage, pabyData + nLineOffset * (size_t)nBlockYOff,
               static_cast<size_t>(nPixelOffset) * nBlockXSize);
    }
    else
    {
        GByte *const pabyCur =
            pabyData + nLineOffset * static_cast<size_t>(nBlockYOff);

        for (int iPixel = 0; iPixel < nBlockXSize; iPixel++)
        {
            memcpy(static_cast<GByte *>(pImage) + iPixel * nWordSize,
                   pabyCur + iPixel * nPixelOffset, nWordSize);
        }
    }

    return CE_None;
}

/************************************************************************/
/*                            IWriteBlock()                             */
/************************************************************************/

CPLErr MEMRasterBand::IWriteBlock(CPL_UNUSED int nBlockXOff, int nBlockYOff,
                                  void *pImage)
{
    CPLAssert(nBlockXOff == 0);
    const int nWordSize = GDALGetDataTypeSize(eDataType) / 8;

    if (nPixelOffset == nWordSize)
    {
        memcpy(pabyData + nLineOffset * (size_t)nBlockYOff, pImage,
               static_cast<size_t>(nPixelOffset) * nBlockXSize);
    }
    else
    {
        GByte *pabyCur =
            pabyData + nLineOffset * static_cast<size_t>(nBlockYOff);

        for (int iPixel = 0; iPixel < nBlockXSize; iPixel++)
        {
            memcpy(pabyCur + iPixel * nPixelOffset,
                   static_cast<GByte *>(pImage) + iPixel * nWordSize,
                   nWordSize);
        }
    }

    return CE_None;
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr MEMRasterBand::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                                int nXSize, int nYSize, void *pData,
                                int nBufXSize, int nBufYSize,
                                GDALDataType eBufType, GSpacing nPixelSpaceBuf,
                                GSpacing nLineSpaceBuf,
                                GDALRasterIOExtraArg *psExtraArg)
{
    if (nXSize != nBufXSize || nYSize != nBufYSize)
    {
        return GDALRasterBand::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                         pData, nBufXSize, nBufYSize, eBufType,
                                         static_cast<int>(nPixelSpaceBuf),
                                         nLineSpaceBuf, psExtraArg);
    }

    // In case block based I/O has been done before.
    FlushCache(false);

    if (eRWFlag == GF_Read)
    {
        for (int iLine = 0; iLine < nYSize; iLine++)
        {
            GDALCopyWords(pabyData +
                              nLineOffset *
                                  static_cast<GPtrDiff_t>(iLine + nYOff) +
                              nXOff * nPixelOffset,
                          eDataType, static_cast<int>(nPixelOffset),
                          static_cast<GByte *>(pData) +
                              nLineSpaceBuf * static_cast<GPtrDiff_t>(iLine),
                          eBufType, static_cast<int>(nPixelSpaceBuf), nXSize);
        }
    }
    else
    {
        for (int iLine = 0; iLine < nYSize; iLine++)
        {
            GDALCopyWords(static_cast<GByte *>(pData) +
                              nLineSpaceBuf * static_cast<GPtrDiff_t>(iLine),
                          eBufType, static_cast<int>(nPixelSpaceBuf),
                          pabyData +
                              nLineOffset *
                                  static_cast<GPtrDiff_t>(iLine + nYOff) +
                              nXOff * nPixelOffset,
                          eDataType, static_cast<int>(nPixelOffset), nXSize);
        }
    }
    return CE_None;
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr MEMDataset::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                             int nXSize, int nYSize, void *pData, int nBufXSize,
                             int nBufYSize, GDALDataType eBufType,
                             int nBandCount, BANDMAP_TYPE panBandMap,
                             GSpacing nPixelSpaceBuf, GSpacing nLineSpaceBuf,
                             GSpacing nBandSpaceBuf,
                             GDALRasterIOExtraArg *psExtraArg)
{
    const int eBufTypeSize = GDALGetDataTypeSize(eBufType) / 8;

    // Detect if we have a pixel-interleaved buffer
    if (nXSize == nBufXSize && nYSize == nBufYSize && nBandCount == nBands &&
        nBands > 1 && nBandSpaceBuf == eBufTypeSize &&
        nPixelSpaceBuf == nBandSpaceBuf * nBands)
    {
        const auto IsPixelInterleaveDataset = [this, nBandCount, panBandMap]()
        {
            GDALDataType eDT = GDT_Unknown;
            GByte *pabyData = nullptr;
            GSpacing nPixelOffset = 0;
            GSpacing nLineOffset = 0;
            int eDTSize = 0;
            for (int iBandIndex = 0; iBandIndex < nBandCount; iBandIndex++)
            {
                if (panBandMap[iBandIndex] != iBandIndex + 1)
                    return false;

                MEMRasterBand *poBand = cpl::down_cast<MEMRasterBand *>(
                    GetRasterBand(iBandIndex + 1));
                if (iBandIndex == 0)
                {
                    eDT = poBand->GetRasterDataType();
                    pabyData = poBand->pabyData;
                    nPixelOffset = poBand->nPixelOffset;
                    nLineOffset = poBand->nLineOffset;
                    eDTSize = GDALGetDataTypeSizeBytes(eDT);
                    if (nPixelOffset != static_cast<GSpacing>(nBands) * eDTSize)
                        return false;
                }
                else if (poBand->GetRasterDataType() != eDT ||
                         nPixelOffset != poBand->nPixelOffset ||
                         nLineOffset != poBand->nLineOffset ||
                         poBand->pabyData != pabyData + iBandIndex * eDTSize)
                {
                    return false;
                }
            }
            return true;
        };

        const auto IsBandSeparatedDataset = [this, nBandCount, panBandMap]()
        {
            GDALDataType eDT = GDT_Unknown;
            GSpacing nPixelOffset = 0;
            GSpacing nLineOffset = 0;
            int eDTSize = 0;
            for (int iBandIndex = 0; iBandIndex < nBandCount; iBandIndex++)
            {
                if (panBandMap[iBandIndex] != iBandIndex + 1)
                    return false;

                MEMRasterBand *poBand = cpl::down_cast<MEMRasterBand *>(
                    GetRasterBand(iBandIndex + 1));
                if (iBandIndex == 0)
                {
                    eDT = poBand->GetRasterDataType();
                    nPixelOffset = poBand->nPixelOffset;
                    nLineOffset = poBand->nLineOffset;
                    eDTSize = GDALGetDataTypeSizeBytes(eDT);
                    if (nPixelOffset != eDTSize)
                        return false;
                }
                else if (poBand->GetRasterDataType() != eDT ||
                         nPixelOffset != poBand->nPixelOffset ||
                         nLineOffset != poBand->nLineOffset)
                {
                    return false;
                }
            }
            return true;
        };

        if (IsPixelInterleaveDataset())
        {
            FlushCache(false);
            const auto poFirstBand =
                cpl::down_cast<MEMRasterBand *>(papoBands[0]);
            const GDALDataType eDT = poFirstBand->GetRasterDataType();
            GByte *pabyData = poFirstBand->pabyData;
            const GSpacing nPixelOffset = poFirstBand->nPixelOffset;
            const GSpacing nLineOffset = poFirstBand->nLineOffset;
            const int eDTSize = GDALGetDataTypeSizeBytes(eDT);
            if (eRWFlag == GF_Read)
            {
                for (int iLine = 0; iLine < nYSize; iLine++)
                {
                    GDALCopyWords(
                        pabyData +
                            nLineOffset * static_cast<size_t>(iLine + nYOff) +
                            nXOff * nPixelOffset,
                        eDT, eDTSize,
                        static_cast<GByte *>(pData) +
                            nLineSpaceBuf * static_cast<size_t>(iLine),
                        eBufType, eBufTypeSize, nXSize * nBands);
                }
            }
            else
            {
                for (int iLine = 0; iLine < nYSize; iLine++)
                {
                    GDALCopyWords(static_cast<GByte *>(pData) +
                                      nLineSpaceBuf * (size_t)iLine,
                                  eBufType, eBufTypeSize,
                                  pabyData +
                                      nLineOffset *
                                          static_cast<size_t>(iLine + nYOff) +
                                      nXOff * nPixelOffset,
                                  eDT, eDTSize, nXSize * nBands);
                }
            }
            return CE_None;
        }
        else if (eRWFlag == GF_Write && nBandCount <= 4 &&
                 IsBandSeparatedDataset())
        {
            // TODO: once we have a GDALInterleave() function, implement the
            // GF_Read case
            FlushCache(false);
            const auto poFirstBand =
                cpl::down_cast<MEMRasterBand *>(papoBands[0]);
            const GDALDataType eDT = poFirstBand->GetRasterDataType();
            void *ppDestBuffer[4] = {nullptr, nullptr, nullptr, nullptr};
            if (nXOff == 0 && nXSize == nRasterXSize &&
                poFirstBand->nLineOffset ==
                    poFirstBand->nPixelOffset * nXSize &&
                nLineSpaceBuf == nPixelSpaceBuf * nXSize)
            {
                // Optimization of the general case in the below else() clause:
                // writing whole strips from a fully packed buffer
                for (int i = 0; i < nBandCount; ++i)
                {
                    const auto poBand =
                        cpl::down_cast<MEMRasterBand *>(papoBands[i]);
                    ppDestBuffer[i] =
                        poBand->pabyData + poBand->nLineOffset * nYOff;
                }
                GDALDeinterleave(pData, eBufType, nBandCount, ppDestBuffer, eDT,
                                 static_cast<size_t>(nXSize) * nYSize);
            }
            else
            {
                for (int iLine = 0; iLine < nYSize; iLine++)
                {
                    for (int i = 0; i < nBandCount; ++i)
                    {
                        const auto poBand =
                            cpl::down_cast<MEMRasterBand *>(papoBands[i]);
                        ppDestBuffer[i] = poBand->pabyData +
                                          poBand->nPixelOffset * nXOff +
                                          poBand->nLineOffset * (iLine + nYOff);
                    }
                    GDALDeinterleave(
                        static_cast<GByte *>(pData) +
                            nLineSpaceBuf * static_cast<size_t>(iLine),
                        eBufType, nBandCount, ppDestBuffer, eDT, nXSize);
                }
            }
            return CE_None;
        }
    }

    if (nBufXSize != nXSize || nBufYSize != nYSize)
        return GDALDataset::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                      pData, nBufXSize, nBufYSize, eBufType,
                                      nBandCount, panBandMap, nPixelSpaceBuf,
                                      nLineSpaceBuf, nBandSpaceBuf, psExtraArg);

    return GDALDataset::BandBasedRasterIO(
        eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize, nBufYSize,
        eBufType, nBandCount, panBandMap, nPixelSpaceBuf, nLineSpaceBuf,
        nBandSpaceBuf, psExtraArg);
}

/************************************************************************/
/*                          GetOverviewCount()                          */
/************************************************************************/

int MEMRasterBand::GetOverviewCount()
{
    MEMDataset *poMemDS = dynamic_cast<MEMDataset *>(poDS);
    if (poMemDS == nullptr)
        return 0;
    return static_cast<int>(poMemDS->m_apoOverviewDS.size());
}

/************************************************************************/
/*                            GetOverview()                             */
/************************************************************************/

GDALRasterBand *MEMRasterBand::GetOverview(int i)

{
    MEMDataset *poMemDS = dynamic_cast<MEMDataset *>(poDS);
    if (poMemDS == nullptr)
        return nullptr;
    if (i < 0 || i >= static_cast<int>(poMemDS->m_apoOverviewDS.size()))
        return nullptr;
    return poMemDS->m_apoOverviewDS[i]->GetRasterBand(nBand);
}

/************************************************************************/
/*                         CreateMaskBand()                             */
/************************************************************************/

CPLErr MEMRasterBand::CreateMaskBand(int nFlagsIn)
{
    InvalidateMaskBand();

    MEMDataset *poMemDS = dynamic_cast<MEMDataset *>(poDS);
    if ((nFlagsIn & GMF_PER_DATASET) != 0 && nBand != 1 && poMemDS != nullptr)
    {
        MEMRasterBand *poFirstBand =
            dynamic_cast<MEMRasterBand *>(poMemDS->GetRasterBand(1));
        if (poFirstBand != nullptr)
            return poFirstBand->CreateMaskBand(nFlagsIn);
    }

    GByte *pabyMaskData =
        static_cast<GByte *>(VSI_CALLOC_VERBOSE(nRasterXSize, nRasterYSize));
    if (pabyMaskData == nullptr)
        return CE_Failure;

    nMaskFlags = nFlagsIn;
    auto poMemMaskBand = new MEMRasterBand(pabyMaskData, GDT_Byte, nRasterXSize,
                                           nRasterYSize, /* bOwnData= */ true);
    poMemMaskBand->m_bIsMask = true;
    poMask.reset(poMemMaskBand, true);
    if ((nFlagsIn & GMF_PER_DATASET) != 0 && nBand == 1 && poMemDS != nullptr)
    {
        for (int i = 2; i <= poMemDS->GetRasterCount(); ++i)
        {
            MEMRasterBand *poOtherBand =
                cpl::down_cast<MEMRasterBand *>(poMemDS->GetRasterBand(i));
            poOtherBand->InvalidateMaskBand();
            poOtherBand->nMaskFlags = nFlagsIn;
            poOtherBand->poMask.reset(poMask.get(), false);
        }
    }
    return CE_None;
}

/************************************************************************/
/*                            IsMaskBand()                              */
/************************************************************************/

bool MEMRasterBand::IsMaskBand() const
{
    return m_bIsMask || GDALPamRasterBand::IsMaskBand();
}

/************************************************************************/
/* ==================================================================== */
/*      MEMDataset                                                     */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                            MEMDataset()                             */
/************************************************************************/

MEMDataset::MEMDataset()
    : GDALDataset(FALSE), bGeoTransformSet(FALSE), m_poPrivate(new Private())
{
    adfGeoTransform[0] = 0.0;
    adfGeoTransform[1] = 1.0;
    adfGeoTransform[2] = 0.0;
    adfGeoTransform[3] = 0.0;
    adfGeoTransform[4] = 0.0;
    adfGeoTransform[5] = -1.0;
    DisableReadWriteMutex();
}

/************************************************************************/
/*                            ~MEMDataset()                            */
/************************************************************************/

MEMDataset::~MEMDataset()

{
    const bool bSuppressOnCloseBackup = bSuppressOnClose;
    bSuppressOnClose = true;
    FlushCache(true);
    bSuppressOnClose = bSuppressOnCloseBackup;
}

#if 0
/************************************************************************/
/*                          EnterReadWrite()                            */
/************************************************************************/

int MEMDataset::EnterReadWrite(CPL_UNUSED GDALRWFlag eRWFlag)
{
    return TRUE;
}

/************************************************************************/
/*                         LeaveReadWrite()                             */
/************************************************************************/

void MEMDataset::LeaveReadWrite()
{
}
#endif  // if 0

/************************************************************************/
/*                          GetSpatialRef()                             */
/************************************************************************/

const OGRSpatialReference *MEMDataset::GetSpatialRef() const

{
    return m_oSRS.IsEmpty() ? nullptr : &m_oSRS;
}

/************************************************************************/
/*                           SetSpatialRef()                            */
/************************************************************************/

CPLErr MEMDataset::SetSpatialRef(const OGRSpatialReference *poSRS)

{
    m_oSRS.Clear();
    if (poSRS)
        m_oSRS = *poSRS;

    return CE_None;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr MEMDataset::GetGeoTransform(double *padfGeoTransform)

{
    memcpy(padfGeoTransform, adfGeoTransform, sizeof(double) * 6);
    if (bGeoTransformSet)
        return CE_None;

    return CE_Failure;
}

/************************************************************************/
/*                          SetGeoTransform()                           */
/************************************************************************/

CPLErr MEMDataset::SetGeoTransform(double *padfGeoTransform)

{
    memcpy(adfGeoTransform, padfGeoTransform, sizeof(double) * 6);
    bGeoTransformSet = TRUE;

    return CE_None;
}

/************************************************************************/
/*                          GetInternalHandle()                         */
/************************************************************************/

void *MEMDataset::GetInternalHandle(const char *pszRequest)

{
    // check for MEMORYnnn string in pszRequest (nnnn can be up to 10
    // digits, or even omitted)
    if (STARTS_WITH_CI(pszRequest, "MEMORY"))
    {
        if (int BandNumber = static_cast<int>(CPLScanLong(&pszRequest[6], 10)))
        {
            MEMRasterBand *RequestedRasterBand =
                cpl::down_cast<MEMRasterBand *>(GetRasterBand(BandNumber));

            // we're within a MEMDataset so the only thing a RasterBand
            // could be is a MEMRasterBand

            if (RequestedRasterBand != nullptr)
            {
                // return the internal band data pointer
                return RequestedRasterBand->GetData();
            }
        }
    }

    return nullptr;
}

/************************************************************************/
/*                            GetGCPCount()                             */
/************************************************************************/

int MEMDataset::GetGCPCount()

{
    return static_cast<int>(m_aoGCPs.size());
}

/************************************************************************/
/*                          GetGCPSpatialRef()                          */
/************************************************************************/

const OGRSpatialReference *MEMDataset::GetGCPSpatialRef() const

{
    return m_oGCPSRS.IsEmpty() ? nullptr : &m_oGCPSRS;
}

/************************************************************************/
/*                              GetGCPs()                               */
/************************************************************************/

const GDAL_GCP *MEMDataset::GetGCPs()

{
    return gdal::GCP::c_ptr(m_aoGCPs);
}

/************************************************************************/
/*                              SetGCPs()                               */
/************************************************************************/

CPLErr MEMDataset::SetGCPs(int nNewCount, const GDAL_GCP *pasNewGCPList,
                           const OGRSpatialReference *poSRS)

{
    m_oGCPSRS.Clear();
    if (poSRS)
        m_oGCPSRS = *poSRS;

    m_aoGCPs = gdal::GCP::fromC(pasNewGCPList, nNewCount);

    return CE_None;
}

/************************************************************************/
/*                              AddBand()                               */
/*                                                                      */
/*      Add a new band to the dataset, allowing creation options to     */
/*      specify the existing memory to use, otherwise create new        */
/*      memory.                                                         */
/************************************************************************/

CPLErr MEMDataset::AddBand(GDALDataType eType, char **papszOptions)

{
    const int nBandId = GetRasterCount() + 1;
    const GSpacing nPixelSize = GDALGetDataTypeSizeBytes(eType);
    if (nPixelSize == 0)
    {
        ReportError(CE_Failure, CPLE_IllegalArg,
                    "Illegal GDT_Unknown/GDT_TypeCount argument");
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Do we need to allocate the memory ourselves?  This is the       */
    /*      simple case.                                                    */
    /* -------------------------------------------------------------------- */
    if (CSLFetchNameValue(papszOptions, "DATAPOINTER") == nullptr)
    {
        const GSpacing nTmp = nPixelSize * GetRasterXSize();
        GByte *pData =
#if SIZEOF_VOIDP == 4
            (nTmp > INT_MAX) ? nullptr :
#endif
                             static_cast<GByte *>(VSI_CALLOC_VERBOSE(
                                 (size_t)nTmp, GetRasterYSize()));

        if (pData == nullptr)
        {
            return CE_Failure;
        }

        SetBand(nBandId,
                new MEMRasterBand(this, nBandId, pData, eType, nPixelSize,
                                  nPixelSize * GetRasterXSize(), TRUE));

        return CE_None;
    }

    /* -------------------------------------------------------------------- */
    /*      Get layout of memory and other flags.                           */
    /* -------------------------------------------------------------------- */
    const char *pszDataPointer = CSLFetchNameValue(papszOptions, "DATAPOINTER");
    GByte *pData = static_cast<GByte *>(CPLScanPointer(
        pszDataPointer, static_cast<int>(strlen(pszDataPointer))));

    const char *pszOption = CSLFetchNameValue(papszOptions, "PIXELOFFSET");
    GSpacing nPixelOffset;
    if (pszOption == nullptr)
        nPixelOffset = nPixelSize;
    else
        nPixelOffset = CPLAtoGIntBig(pszOption);

    pszOption = CSLFetchNameValue(papszOptions, "LINEOFFSET");
    GSpacing nLineOffset;
    if (pszOption == nullptr)
        nLineOffset = GetRasterXSize() * static_cast<size_t>(nPixelOffset);
    else
        nLineOffset = CPLAtoGIntBig(pszOption);

    SetBand(nBandId, new MEMRasterBand(this, nBandId, pData, eType,
                                       nPixelOffset, nLineOffset, FALSE));

    return CE_None;
}

/************************************************************************/
/*                           AddMEMBand()                               */
/************************************************************************/

void MEMDataset::AddMEMBand(GDALRasterBandH hMEMBand)
{
    auto poBand = GDALRasterBand::FromHandle(hMEMBand);
    CPLAssert(dynamic_cast<MEMRasterBand *>(poBand) != nullptr);
    SetBand(1 + nBands, poBand);
}

/************************************************************************/
/*                          IBuildOverviews()                           */
/************************************************************************/

CPLErr MEMDataset::IBuildOverviews(const char *pszResampling, int nOverviews,
                                   const int *panOverviewList, int nListBands,
                                   const int *panBandList,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData,
                                   CSLConstList papszOptions)
{
    if (nBands == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Dataset has zero bands.");
        return CE_Failure;
    }

    if (nListBands != nBands)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Generation of overviews in MEM only"
                 "supported when operating on all bands.");
        return CE_Failure;
    }

    if (nOverviews == 0)
    {
        // Cleanup existing overviews
        m_apoOverviewDS.clear();
        return CE_None;
    }

    /* -------------------------------------------------------------------- */
    /*      Force cascading. Help to get accurate results when masks are    */
    /*      involved.                                                       */
    /* -------------------------------------------------------------------- */
    if (nOverviews > 1 &&
        (STARTS_WITH_CI(pszResampling, "AVER") ||
         STARTS_WITH_CI(pszResampling, "GAUSS") ||
         EQUAL(pszResampling, "CUBIC") || EQUAL(pszResampling, "CUBICSPLINE") ||
         EQUAL(pszResampling, "LANCZOS") || EQUAL(pszResampling, "BILINEAR")))
    {
        double dfTotalPixels = 0;
        for (int i = 0; i < nOverviews; i++)
        {
            dfTotalPixels += static_cast<double>(nRasterXSize) * nRasterYSize /
                             (panOverviewList[i] * panOverviewList[i]);
        }

        double dfAccPixels = 0;
        for (int i = 0; i < nOverviews; i++)
        {
            double dfPixels = static_cast<double>(nRasterXSize) * nRasterYSize /
                              (panOverviewList[i] * panOverviewList[i]);
            void *pScaledProgress = GDALCreateScaledProgress(
                dfAccPixels / dfTotalPixels,
                (dfAccPixels + dfPixels) / dfTotalPixels, pfnProgress,
                pProgressData);
            CPLErr eErr = IBuildOverviews(
                pszResampling, 1, &panOverviewList[i], nListBands, panBandList,
                GDALScaledProgress, pScaledProgress, papszOptions);
            GDALDestroyScaledProgress(pScaledProgress);
            dfAccPixels += dfPixels;
            if (eErr == CE_Failure)
                return eErr;
        }
        return CE_None;
    }

    /* -------------------------------------------------------------------- */
    /*      Establish which of the overview levels we already have, and     */
    /*      which are new.                                                  */
    /* -------------------------------------------------------------------- */
    GDALRasterBand *poBand = GetRasterBand(1);

    for (int i = 0; i < nOverviews; i++)
    {
        bool bExisting = false;
        for (int j = 0; j < poBand->GetOverviewCount(); j++)
        {
            GDALRasterBand *poOverview = poBand->GetOverview(j);
            if (poOverview == nullptr)
                continue;

            int nOvFactor =
                GDALComputeOvFactor(poOverview->GetXSize(), poBand->GetXSize(),
                                    poOverview->GetYSize(), poBand->GetYSize());

            if (nOvFactor == panOverviewList[i] ||
                nOvFactor == GDALOvLevelAdjust2(panOverviewList[i],
                                                poBand->GetXSize(),
                                                poBand->GetYSize()))
            {
                bExisting = true;
                break;
            }
        }

        // Create new overview dataset if needed.
        if (!bExisting)
        {
            auto poOvrDS = std::make_unique<MEMDataset>();
            poOvrDS->eAccess = GA_Update;
            poOvrDS->nRasterXSize =
                DIV_ROUND_UP(nRasterXSize, panOverviewList[i]);
            poOvrDS->nRasterYSize =
                DIV_ROUND_UP(nRasterYSize, panOverviewList[i]);
            for (int iBand = 0; iBand < nBands; iBand++)
            {
                const GDALDataType eDT =
                    GetRasterBand(iBand + 1)->GetRasterDataType();
                if (poOvrDS->AddBand(eDT, nullptr) != CE_None)
                {
                    return CE_Failure;
                }
            }
            m_apoOverviewDS.emplace_back(std::move(poOvrDS));
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Build band list.                                                */
    /* -------------------------------------------------------------------- */
    GDALRasterBand **pahBands = static_cast<GDALRasterBand **>(
        CPLCalloc(sizeof(GDALRasterBand *), nBands));
    for (int i = 0; i < nBands; i++)
        pahBands[i] = GetRasterBand(panBandList[i]);

    /* -------------------------------------------------------------------- */
    /*      Refresh overviews that were listed.                             */
    /* -------------------------------------------------------------------- */
    GDALRasterBand **papoOverviewBands =
        static_cast<GDALRasterBand **>(CPLCalloc(sizeof(void *), nOverviews));
    GDALRasterBand **papoMaskOverviewBands =
        static_cast<GDALRasterBand **>(CPLCalloc(sizeof(void *), nOverviews));

    CPLErr eErr = CE_None;
    for (int iBand = 0; iBand < nBands && eErr == CE_None; iBand++)
    {
        poBand = GetRasterBand(panBandList[iBand]);

        int nNewOverviews = 0;
        for (int i = 0; i < nOverviews; i++)
        {
            for (int j = 0; j < poBand->GetOverviewCount(); j++)
            {
                GDALRasterBand *poOverview = poBand->GetOverview(j);

                int bHasNoData = FALSE;
                double noDataValue = poBand->GetNoDataValue(&bHasNoData);

                if (bHasNoData)
                    poOverview->SetNoDataValue(noDataValue);

                const int nOvFactor = GDALComputeOvFactor(
                    poOverview->GetXSize(), poBand->GetXSize(),
                    poOverview->GetYSize(), poBand->GetYSize());

                if (nOvFactor == panOverviewList[i] ||
                    nOvFactor == GDALOvLevelAdjust2(panOverviewList[i],
                                                    poBand->GetXSize(),
                                                    poBand->GetYSize()))
                {
                    papoOverviewBands[nNewOverviews++] = poOverview;
                    break;
                }
            }
        }

        // If the band has an explicit mask, we need to create overviews
        // for it
        MEMRasterBand *poMEMBand = cpl::down_cast<MEMRasterBand *>(poBand);
        const bool bMustGenerateMaskOvr =
            ((poMEMBand->poMask != nullptr && poMEMBand->poMask.IsOwned()) ||
             // Or if it is a per-dataset mask, in which case just do it for the
             // first band
             ((poMEMBand->nMaskFlags & GMF_PER_DATASET) != 0 && iBand == 0)) &&
            dynamic_cast<MEMRasterBand *>(poBand->GetMaskBand()) != nullptr;

        if (nNewOverviews > 0 && bMustGenerateMaskOvr)
        {
            for (int i = 0; i < nNewOverviews; i++)
            {
                MEMRasterBand *poMEMOvrBand =
                    cpl::down_cast<MEMRasterBand *>(papoOverviewBands[i]);
                if (!(poMEMOvrBand->poMask != nullptr &&
                      poMEMOvrBand->poMask.IsOwned()) &&
                    (poMEMOvrBand->nMaskFlags & GMF_PER_DATASET) == 0)
                {
                    poMEMOvrBand->CreateMaskBand(poMEMBand->nMaskFlags);
                }
                papoMaskOverviewBands[i] = poMEMOvrBand->GetMaskBand();
            }

            void *pScaledProgress = GDALCreateScaledProgress(
                1.0 * iBand / nBands, 1.0 * (iBand + 0.5) / nBands, pfnProgress,
                pProgressData);

            MEMRasterBand *poMaskBand =
                cpl::down_cast<MEMRasterBand *>(poBand->GetMaskBand());
            // Make the mask band to be its own mask, similarly to what is
            // done for alpha bands in GDALRegenerateOverviews() (#5640)
            poMaskBand->InvalidateMaskBand();
            poMaskBand->poMask.reset(poMaskBand, false);
            poMaskBand->nMaskFlags = 0;
            eErr = GDALRegenerateOverviewsEx(
                (GDALRasterBandH)poMaskBand, nNewOverviews,
                (GDALRasterBandH *)papoMaskOverviewBands, pszResampling,
                GDALScaledProgress, pScaledProgress, papszOptions);
            poMaskBand->InvalidateMaskBand();
            GDALDestroyScaledProgress(pScaledProgress);
        }

        // Generate overview of bands *AFTER* mask overviews
        if (nNewOverviews > 0 && eErr == CE_None)
        {
            void *pScaledProgress = GDALCreateScaledProgress(
                1.0 * (iBand + (bMustGenerateMaskOvr ? 0.5 : 1)) / nBands,
                1.0 * (iBand + 1) / nBands, pfnProgress, pProgressData);
            eErr = GDALRegenerateOverviewsEx(
                (GDALRasterBandH)poBand, nNewOverviews,
                (GDALRasterBandH *)papoOverviewBands, pszResampling,
                GDALScaledProgress, pScaledProgress, papszOptions);
            GDALDestroyScaledProgress(pScaledProgress);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Cleanup                                                         */
    /* -------------------------------------------------------------------- */
    CPLFree(papoOverviewBands);
    CPLFree(papoMaskOverviewBands);
    CPLFree(pahBands);

    return eErr;
}

/************************************************************************/
/*                         CreateMaskBand()                             */
/************************************************************************/

CPLErr MEMDataset::CreateMaskBand(int nFlagsIn)
{
    GDALRasterBand *poFirstBand = GetRasterBand(1);
    if (poFirstBand == nullptr)
        return CE_Failure;
    return poFirstBand->CreateMaskBand(nFlagsIn | GMF_PER_DATASET);
}

/************************************************************************/
/*                           CanBeCloned()                              */
/************************************************************************/

/** Implements GDALDataset::CanBeCloned()
 *
 * This method is called by GDALThreadSafeDataset::Create() to determine if
 * it is possible to create a thread-safe wrapper for a dataset, which involves
 * the ability to Clone() it.
 *
 * The implementation of this method must be thread-safe.
 */
bool MEMDataset::CanBeCloned(int nScopeFlags, bool bCanShareState) const
{
    return nScopeFlags == GDAL_OF_RASTER && bCanShareState &&
           typeid(this) == typeid(const MEMDataset *);
}

/************************************************************************/
/*                              Clone()                                 */
/************************************************************************/

/** Implements GDALDataset::Clone()
 *
 * This method returns a new instance, identical to "this", but which shares the
 * same memory buffer as "this".
 *
 * The implementation of this method must be thread-safe.
 */
std::unique_ptr<GDALDataset> MEMDataset::Clone(int nScopeFlags,
                                               bool bCanShareState) const
{
    if (MEMDataset::CanBeCloned(nScopeFlags, bCanShareState))
    {
        auto poNewDS = std::make_unique<MEMDataset>();
        poNewDS->poDriver = poDriver;
        poNewDS->nRasterXSize = nRasterXSize;
        poNewDS->nRasterYSize = nRasterYSize;
        poNewDS->bGeoTransformSet = bGeoTransformSet;
        memcpy(poNewDS->adfGeoTransform, adfGeoTransform,
               sizeof(adfGeoTransform));
        poNewDS->m_oSRS = m_oSRS;
        poNewDS->m_aoGCPs = m_aoGCPs;
        poNewDS->m_oGCPSRS = m_oGCPSRS;
        for (const auto &poOvrDS : m_apoOverviewDS)
        {
            poNewDS->m_apoOverviewDS.emplace_back(
                poOvrDS->Clone(nScopeFlags, bCanShareState));
        }

        poNewDS->SetDescription(GetDescription());
        poNewDS->oMDMD = oMDMD;

        // Clone bands
        for (int i = 1; i <= nBands; ++i)
        {
            auto poSrcMEMBand =
                dynamic_cast<const MEMRasterBand *>(papoBands[i - 1]);
            CPLAssert(poSrcMEMBand);
            auto poNewBand = std::make_unique<MEMRasterBand>(
                poNewDS.get(), i, poSrcMEMBand->pabyData,
                poSrcMEMBand->GetRasterDataType(), poSrcMEMBand->nPixelOffset,
                poSrcMEMBand->nLineOffset,
                /* bAssumeOwnership = */ false);

            poNewBand->SetDescription(poSrcMEMBand->GetDescription());
            poNewBand->oMDMD = poSrcMEMBand->oMDMD;

            if (poSrcMEMBand->psPam)
            {
                poNewBand->PamInitialize();
                CPLAssert(poNewBand->psPam);
                poNewBand->psPam->CopyFrom(*(poSrcMEMBand->psPam));
            }

            // Instantiates a mask band when needed.
            if ((poSrcMEMBand->nMaskFlags &
                 (GMF_ALL_VALID | GMF_ALPHA | GMF_NODATA)) == 0)
            {
                auto poSrcMaskBand = dynamic_cast<const MEMRasterBand *>(
                    poSrcMEMBand->poMask.get());
                if (poSrcMaskBand)
                {
                    auto poMaskBand = new MEMRasterBand(
                        poSrcMaskBand->pabyData, GDT_Byte, nRasterXSize,
                        nRasterYSize, /* bOwnData = */ false);
                    poMaskBand->m_bIsMask = true;
                    poNewBand->poMask.reset(poMaskBand, true);
                    poNewBand->nMaskFlags = poSrcMaskBand->nMaskFlags;
                }
            }

            poNewDS->SetBand(i, std::move(poNewBand));
        }

        return poNewDS;
    }
    return GDALDataset::Clone(nScopeFlags, bCanShareState);
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *MEMDataset::Open(GDALOpenInfo *poOpenInfo)

{
    /* -------------------------------------------------------------------- */
    /*      Do we have the special filename signature for MEM format        */
    /*      description strings?                                            */
    /* -------------------------------------------------------------------- */
    if (!STARTS_WITH_CI(poOpenInfo->pszFilename, "MEM:::") ||
        poOpenInfo->fpL != nullptr)
        return nullptr;

#ifndef GDAL_MEM_ENABLE_OPEN
    if (!CPLTestBool(CPLGetConfigOption("GDAL_MEM_ENABLE_OPEN", "NO")))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Opening a MEM dataset with the MEM:::DATAPOINTER= syntax "
                 "is no longer supported by default for security reasons. "
                 "If you want to allow it, define the "
                 "GDAL_MEM_ENABLE_OPEN "
                 "configuration option to YES, or build GDAL with the "
                 "GDAL_MEM_ENABLE_OPEN compilation definition");
        return nullptr;
    }
#endif

    char **papszOptions =
        CSLTokenizeStringComplex(poOpenInfo->pszFilename + 6, ",", TRUE, FALSE);

    /* -------------------------------------------------------------------- */
    /*      Verify we have all required fields                              */
    /* -------------------------------------------------------------------- */
    if (CSLFetchNameValue(papszOptions, "PIXELS") == nullptr ||
        CSLFetchNameValue(papszOptions, "LINES") == nullptr ||
        CSLFetchNameValue(papszOptions, "DATAPOINTER") == nullptr)
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "Missing required field (one of PIXELS, LINES or DATAPOINTER).  "
            "Unable to access in-memory array.");

        CSLDestroy(papszOptions);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Create the new MEMDataset object.                               */
    /* -------------------------------------------------------------------- */
    MEMDataset *poDS = new MEMDataset();

    poDS->nRasterXSize = atoi(CSLFetchNameValue(papszOptions, "PIXELS"));
    poDS->nRasterYSize = atoi(CSLFetchNameValue(papszOptions, "LINES"));
    poDS->eAccess = poOpenInfo->eAccess;

    /* -------------------------------------------------------------------- */
    /*      Extract other information.                                      */
    /* -------------------------------------------------------------------- */
    const char *pszOption = CSLFetchNameValue(papszOptions, "BANDS");
    int nBands = 1;
    if (pszOption != nullptr)
    {
        nBands = atoi(pszOption);
    }

    if (!GDALCheckDatasetDimensions(poDS->nRasterXSize, poDS->nRasterYSize) ||
        !GDALCheckBandCount(nBands, TRUE))
    {
        CSLDestroy(papszOptions);
        delete poDS;
        return nullptr;
    }

    pszOption = CSLFetchNameValue(papszOptions, "DATATYPE");
    GDALDataType eType = GDT_Byte;
    if (pszOption != nullptr)
    {
        if (atoi(pszOption) > 0 && atoi(pszOption) < GDT_TypeCount)
            eType = static_cast<GDALDataType>(atoi(pszOption));
        else
        {
            eType = GDT_Unknown;
            for (int iType = 0; iType < GDT_TypeCount; iType++)
            {
                if (EQUAL(GDALGetDataTypeName((GDALDataType)iType), pszOption))
                {
                    eType = static_cast<GDALDataType>(iType);
                    break;
                }
            }

            if (eType == GDT_Unknown)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "DATATYPE=%s not recognised.", pszOption);
                CSLDestroy(papszOptions);
                delete poDS;
                return nullptr;
            }
        }
    }

    pszOption = CSLFetchNameValue(papszOptions, "PIXELOFFSET");
    GSpacing nPixelOffset;
    if (pszOption == nullptr)
        nPixelOffset = GDALGetDataTypeSizeBytes(eType);
    else
        nPixelOffset =
            CPLScanUIntBig(pszOption, static_cast<int>(strlen(pszOption)));

    pszOption = CSLFetchNameValue(papszOptions, "LINEOFFSET");
    GSpacing nLineOffset = 0;
    if (pszOption == nullptr)
        nLineOffset = poDS->nRasterXSize * static_cast<size_t>(nPixelOffset);
    else
        nLineOffset =
            CPLScanUIntBig(pszOption, static_cast<int>(strlen(pszOption)));

    pszOption = CSLFetchNameValue(papszOptions, "BANDOFFSET");
    GSpacing nBandOffset = 0;
    if (pszOption == nullptr)
        nBandOffset = nLineOffset * static_cast<size_t>(poDS->nRasterYSize);
    else
        nBandOffset =
            CPLScanUIntBig(pszOption, static_cast<int>(strlen(pszOption)));

    const char *pszDataPointer = CSLFetchNameValue(papszOptions, "DATAPOINTER");
    GByte *pabyData = static_cast<GByte *>(CPLScanPointer(
        pszDataPointer, static_cast<int>(strlen(pszDataPointer))));

    /* -------------------------------------------------------------------- */
    /*      Create band information objects.                                */
    /* -------------------------------------------------------------------- */
    for (int iBand = 0; iBand < nBands; iBand++)
    {
        poDS->SetBand(iBand + 1,
                      new MEMRasterBand(poDS, iBand + 1,
                                        pabyData + iBand * nBandOffset, eType,
                                        nPixelOffset, nLineOffset, FALSE));
    }

    /* -------------------------------------------------------------------- */
    /*      Set GeoTransform information.                                   */
    /* -------------------------------------------------------------------- */

    pszOption = CSLFetchNameValue(papszOptions, "GEOTRANSFORM");
    if (pszOption != nullptr)
    {
        char **values = CSLTokenizeStringComplex(pszOption, "/", TRUE, FALSE);
        if (CSLCount(values) == 6)
        {
            double adfGeoTransform[6] = {0, 0, 0, 0, 0, 0};
            for (size_t i = 0; i < 6; ++i)
            {
                adfGeoTransform[i] = CPLScanDouble(
                    values[i], static_cast<int>(strlen(values[i])));
            }
            poDS->SetGeoTransform(adfGeoTransform);
        }
        CSLDestroy(values);
    }

    /* -------------------------------------------------------------------- */
    /*      Set Projection Information                                      */
    /* -------------------------------------------------------------------- */

    pszOption = CSLFetchNameValue(papszOptions, "SPATIALREFERENCE");
    if (pszOption != nullptr)
    {
        poDS->m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        if (poDS->m_oSRS.SetFromUserInput(pszOption) != OGRERR_NONE)
        {
            CPLError(CE_Warning, CPLE_AppDefined, "Unrecognized crs: %s",
                     pszOption);
        }
    }
    /* -------------------------------------------------------------------- */
    /*      Try to return a regular handle on the file.                     */
    /* -------------------------------------------------------------------- */
    CSLDestroy(papszOptions);
    return poDS;
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/

MEMDataset *MEMDataset::Create(const char * /* pszFilename */, int nXSize,
                               int nYSize, int nBandsIn, GDALDataType eType,
                               char **papszOptions)
{

    /* -------------------------------------------------------------------- */
    /*      Do we want a pixel interleaved buffer?  I mostly care about     */
    /*      this to test pixel interleaved IO in other contexts, but it     */
    /*      could be useful to create a directly accessible buffer for      */
    /*      some apps.                                                      */
    /* -------------------------------------------------------------------- */
    bool bPixelInterleaved = false;
    const char *pszOption = CSLFetchNameValue(papszOptions, "INTERLEAVE");
    if (pszOption && EQUAL(pszOption, "PIXEL"))
        bPixelInterleaved = true;

    /* -------------------------------------------------------------------- */
    /*      First allocate band data, verifying that we can get enough      */
    /*      memory.                                                         */
    /* -------------------------------------------------------------------- */
    const int nWordSize = GDALGetDataTypeSizeBytes(eType);
    if (nBandsIn > 0 && nWordSize > 0 &&
        (nBandsIn > INT_MAX / nWordSize ||
         (GIntBig)nXSize * nYSize > GINTBIG_MAX / (nWordSize * nBandsIn)))
    {
        CPLError(CE_Failure, CPLE_OutOfMemory, "Multiplication overflow");
        return nullptr;
    }

    const GUIntBig nGlobalBigSize =
        static_cast<GUIntBig>(nWordSize) * nBandsIn * nXSize * nYSize;
    const size_t nGlobalSize = static_cast<size_t>(nGlobalBigSize);
#if SIZEOF_VOIDP == 4
    if (static_cast<GUIntBig>(nGlobalSize) != nGlobalBigSize)
    {
        CPLError(CE_Failure, CPLE_OutOfMemory,
                 "Cannot allocate " CPL_FRMT_GUIB " bytes on this platform.",
                 nGlobalBigSize);
        return nullptr;
    }
#endif

    std::vector<GByte *> apbyBandData;
    if (nBandsIn > 0)
    {
        GByte *pabyData =
            static_cast<GByte *>(VSI_CALLOC_VERBOSE(1, nGlobalSize));
        if (!pabyData)
        {
            return nullptr;
        }

        if (bPixelInterleaved)
        {
            for (int iBand = 0; iBand < nBandsIn; iBand++)
            {
                apbyBandData.push_back(pabyData + iBand * nWordSize);
            }
        }
        else
        {
            for (int iBand = 0; iBand < nBandsIn; iBand++)
            {
                apbyBandData.push_back(
                    pabyData +
                    (static_cast<size_t>(nWordSize) * nXSize * nYSize) * iBand);
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Create the new GTiffDataset object.                             */
    /* -------------------------------------------------------------------- */
    MEMDataset *poDS = new MEMDataset();

    poDS->nRasterXSize = nXSize;
    poDS->nRasterYSize = nYSize;
    poDS->eAccess = GA_Update;

    const char *pszPixelType = CSLFetchNameValue(papszOptions, "PIXELTYPE");
    if (pszPixelType && EQUAL(pszPixelType, "SIGNEDBYTE"))
        poDS->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");

    if (bPixelInterleaved)
        poDS->SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
    else
        poDS->SetMetadataItem("INTERLEAVE", "BAND", "IMAGE_STRUCTURE");

    /* -------------------------------------------------------------------- */
    /*      Create band information objects.                                */
    /* -------------------------------------------------------------------- */
    for (int iBand = 0; iBand < nBandsIn; iBand++)
    {
        MEMRasterBand *poNewBand = nullptr;

        if (bPixelInterleaved)
            poNewBand = new MEMRasterBand(
                poDS, iBand + 1, apbyBandData[iBand], eType,
                cpl::fits_on<int>(nWordSize * nBandsIn), 0, iBand == 0);
        else
            poNewBand = new MEMRasterBand(poDS, iBand + 1, apbyBandData[iBand],
                                          eType, 0, 0, iBand == 0);

        poDS->SetBand(iBand + 1, poNewBand);
    }

    /* -------------------------------------------------------------------- */
    /*      Try to return a regular handle on the file.                     */
    /* -------------------------------------------------------------------- */
    return poDS;
}

GDALDataset *MEMDataset::CreateBase(const char *pszFilename, int nXSize,
                                    int nYSize, int nBandsIn,
                                    GDALDataType eType, char **papszOptions)
{
    return Create(pszFilename, nXSize, nYSize, nBandsIn, eType, papszOptions);
}

/************************************************************************/
/*                        ~MEMAttributeHolder()                         */
/************************************************************************/

MEMAttributeHolder::~MEMAttributeHolder() = default;

/************************************************************************/
/*                          RenameAttribute()                           */
/************************************************************************/

bool MEMAttributeHolder::RenameAttribute(const std::string &osOldName,
                                         const std::string &osNewName)
{
    if (m_oMapAttributes.find(osNewName) != m_oMapAttributes.end())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "An attribute with same name already exists");
        return false;
    }
    auto oIter = m_oMapAttributes.find(osOldName);
    if (oIter == m_oMapAttributes.end())
    {
        CPLAssert(false);
        return false;
    }
    auto poAttr = std::move(oIter->second);
    m_oMapAttributes.erase(oIter);
    m_oMapAttributes[osNewName] = std::move(poAttr);
    return true;
}

/************************************************************************/
/*                           GetMDArrayNames()                          */
/************************************************************************/

std::vector<std::string> MEMGroup::GetMDArrayNames(CSLConstList) const
{
    if (!CheckValidAndErrorOutIfNot())
        return {};
    std::vector<std::string> names;
    for (const auto &iter : m_oMapMDArrays)
        names.push_back(iter.first);
    return names;
}

/************************************************************************/
/*                             OpenMDArray()                            */
/************************************************************************/

std::shared_ptr<GDALMDArray> MEMGroup::OpenMDArray(const std::string &osName,
                                                   CSLConstList) const
{
    if (!CheckValidAndErrorOutIfNot())
        return nullptr;
    auto oIter = m_oMapMDArrays.find(osName);
    if (oIter != m_oMapMDArrays.end())
        return oIter->second;
    return nullptr;
}

/************************************************************************/
/*                            GetGroupNames()                           */
/************************************************************************/

std::vector<std::string> MEMGroup::GetGroupNames(CSLConstList) const
{
    if (!CheckValidAndErrorOutIfNot())
        return {};
    std::vector<std::string> names;
    for (const auto &iter : m_oMapGroups)
        names.push_back(iter.first);
    return names;
}

/************************************************************************/
/*                              OpenGroup()                             */
/************************************************************************/

std::shared_ptr<GDALGroup> MEMGroup::OpenGroup(const std::string &osName,
                                               CSLConstList) const
{
    if (!CheckValidAndErrorOutIfNot())
        return nullptr;
    auto oIter = m_oMapGroups.find(osName);
    if (oIter != m_oMapGroups.end())
        return oIter->second;
    return nullptr;
}

/************************************************************************/
/*                              Create()                                */
/************************************************************************/

/*static*/
std::shared_ptr<MEMGroup> MEMGroup::Create(const std::string &osParentName,
                                           const char *pszName)
{
    auto newGroup(
        std::shared_ptr<MEMGroup>(new MEMGroup(osParentName, pszName)));
    newGroup->SetSelf(newGroup);
    if (osParentName.empty())
        newGroup->m_poRootGroupWeak = newGroup;
    return newGroup;
}

/************************************************************************/
/*                             CreateGroup()                            */
/************************************************************************/

std::shared_ptr<GDALGroup> MEMGroup::CreateGroup(const std::string &osName,
                                                 CSLConstList /*papszOptions*/)
{
    if (!CheckValidAndErrorOutIfNot())
        return nullptr;
    if (osName.empty())
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Empty group name not supported");
        return nullptr;
    }
    if (m_oMapGroups.find(osName) != m_oMapGroups.end())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "A group with same name already exists");
        return nullptr;
    }
    auto newGroup = MEMGroup::Create(GetFullName(), osName.c_str());
    newGroup->m_pParent = std::dynamic_pointer_cast<MEMGroup>(m_pSelf.lock());
    newGroup->m_poRootGroupWeak = m_poRootGroupWeak;
    m_oMapGroups[osName] = newGroup;
    return newGroup;
}

/************************************************************************/
/*                             DeleteGroup()                            */
/************************************************************************/

bool MEMGroup::DeleteGroup(const std::string &osName,
                           CSLConstList /*papszOptions*/)
{
    if (!CheckValidAndErrorOutIfNot())
        return false;
    auto oIter = m_oMapGroups.find(osName);
    if (oIter == m_oMapGroups.end())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Group %s is not a sub-group of this group", osName.c_str());
        return false;
    }

    oIter->second->Deleted();
    m_oMapGroups.erase(oIter);
    return true;
}

/************************************************************************/
/*                       NotifyChildrenOfDeletion()                     */
/************************************************************************/

void MEMGroup::NotifyChildrenOfDeletion()
{
    for (const auto &oIter : m_oMapGroups)
        oIter.second->ParentDeleted();
    for (const auto &oIter : m_oMapMDArrays)
        oIter.second->ParentDeleted();
    for (const auto &oIter : m_oMapAttributes)
        oIter.second->ParentDeleted();
    for (const auto &oIter : m_oMapDimensions)
        oIter.second->ParentDeleted();
}

/************************************************************************/
/*                            CreateMDArray()                           */
/************************************************************************/

std::shared_ptr<GDALMDArray> MEMGroup::CreateMDArray(
    const std::string &osName,
    const std::vector<std::shared_ptr<GDALDimension>> &aoDimensions,
    const GDALExtendedDataType &oType, void *pData, CSLConstList papszOptions)
{
    if (!CheckValidAndErrorOutIfNot())
        return nullptr;
    if (osName.empty())
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Empty array name not supported");
        return nullptr;
    }
    if (m_oMapMDArrays.find(osName) != m_oMapMDArrays.end())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "An array with same name already exists");
        return nullptr;
    }
    auto newArray(
        MEMMDArray::Create(GetFullName(), osName, aoDimensions, oType));

    GByte *pabyData = nullptr;
    std::vector<GPtrDiff_t> anStrides;
    if (pData)
    {
        pabyData = static_cast<GByte *>(pData);
        const char *pszStrides = CSLFetchNameValue(papszOptions, "STRIDES");
        if (pszStrides)
        {
            CPLStringList aosStrides(CSLTokenizeString2(pszStrides, ",", 0));
            if (static_cast<size_t>(aosStrides.size()) != aoDimensions.size())
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Invalid number of strides");
                return nullptr;
            }
            for (int i = 0; i < aosStrides.size(); i++)
            {
                const auto nStride = CPLAtoGIntBig(aosStrides[i]);
                anStrides.push_back(static_cast<GPtrDiff_t>(nStride));
            }
        }
    }
    if (!newArray->Init(pabyData, anStrides))
        return nullptr;

    for (auto &poDim : newArray->GetDimensions())
    {
        const auto dim = std::dynamic_pointer_cast<MEMDimension>(poDim);
        if (dim)
            dim->RegisterUsingArray(newArray.get());
    }

    newArray->RegisterGroup(m_pSelf);
    m_oMapMDArrays[osName] = newArray;
    return newArray;
}

std::shared_ptr<GDALMDArray> MEMGroup::CreateMDArray(
    const std::string &osName,
    const std::vector<std::shared_ptr<GDALDimension>> &aoDimensions,
    const GDALExtendedDataType &oType, CSLConstList papszOptions)
{
    void *pData = nullptr;
    const char *pszDataPointer = CSLFetchNameValue(papszOptions, "DATAPOINTER");
    if (pszDataPointer)
    {
        // Will not work on architectures with "capability pointers"
        pData = CPLScanPointer(pszDataPointer,
                               static_cast<int>(strlen(pszDataPointer)));
    }
    return CreateMDArray(osName, aoDimensions, oType, pData, papszOptions);
}

/************************************************************************/
/*                           DeleteMDArray()                            */
/************************************************************************/

bool MEMGroup::DeleteMDArray(const std::string &osName,
                             CSLConstList /*papszOptions*/)
{
    if (!CheckValidAndErrorOutIfNot())
        return false;
    auto oIter = m_oMapMDArrays.find(osName);
    if (oIter == m_oMapMDArrays.end())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Array %s is not an array of this group", osName.c_str());
        return false;
    }

    oIter->second->Deleted();
    m_oMapMDArrays.erase(oIter);
    return true;
}

/************************************************************************/
/*                      MEMGroupCreateMDArray()                         */
/************************************************************************/

// Used by NUMPYMultiDimensionalDataset
std::shared_ptr<GDALMDArray> MEMGroupCreateMDArray(
    GDALGroup *poGroup, const std::string &osName,
    const std::vector<std::shared_ptr<GDALDimension>> &aoDimensions,
    const GDALExtendedDataType &oDataType, void *pData,
    CSLConstList papszOptions)
{
    auto poMemGroup = dynamic_cast<MEMGroup *>(poGroup);
    if (!poMemGroup)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "MEMGroupCreateMDArray(): poGroup not of type MEMGroup");
        return nullptr;
    }
    return poMemGroup->CreateMDArray(osName, aoDimensions, oDataType, pData,
                                     papszOptions);
}

/************************************************************************/
/*                            GetAttribute()                            */
/************************************************************************/

std::shared_ptr<GDALAttribute>
MEMGroup::GetAttribute(const std::string &osName) const
{
    if (!CheckValidAndErrorOutIfNot())
        return nullptr;
    auto oIter = m_oMapAttributes.find(osName);
    if (oIter != m_oMapAttributes.end())
        return oIter->second;
    return nullptr;
}

/************************************************************************/
/*                            GetAttributes()                           */
/************************************************************************/

std::vector<std::shared_ptr<GDALAttribute>>
MEMGroup::GetAttributes(CSLConstList) const
{
    if (!CheckValidAndErrorOutIfNot())
        return {};
    std::vector<std::shared_ptr<GDALAttribute>> oRes;
    for (const auto &oIter : m_oMapAttributes)
    {
        oRes.push_back(oIter.second);
    }
    return oRes;
}

/************************************************************************/
/*                            GetDimensions()                           */
/************************************************************************/

std::vector<std::shared_ptr<GDALDimension>>
MEMGroup::GetDimensions(CSLConstList) const
{
    if (!CheckValidAndErrorOutIfNot())
        return {};
    std::vector<std::shared_ptr<GDALDimension>> oRes;
    for (const auto &oIter : m_oMapDimensions)
    {
        oRes.push_back(oIter.second);
    }
    return oRes;
}

/************************************************************************/
/*                           CreateAttribute()                          */
/************************************************************************/

std::shared_ptr<GDALAttribute>
MEMGroup::CreateAttribute(const std::string &osName,
                          const std::vector<GUInt64> &anDimensions,
                          const GDALExtendedDataType &oDataType, CSLConstList)
{
    if (!CheckValidAndErrorOutIfNot())
        return nullptr;
    if (osName.empty())
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Empty attribute name not supported");
        return nullptr;
    }
    if (m_oMapAttributes.find(osName) != m_oMapAttributes.end())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "An attribute with same name already exists");
        return nullptr;
    }
    auto newAttr(MEMAttribute::Create(
        std::dynamic_pointer_cast<MEMGroup>(m_pSelf.lock()), osName,
        anDimensions, oDataType));
    if (!newAttr)
        return nullptr;
    m_oMapAttributes[osName] = newAttr;
    return newAttr;
}

/************************************************************************/
/*                         DeleteAttribute()                            */
/************************************************************************/

bool MEMGroup::DeleteAttribute(const std::string &osName,
                               CSLConstList /*papszOptions*/)
{
    if (!CheckValidAndErrorOutIfNot())
        return false;
    auto oIter = m_oMapAttributes.find(osName);
    if (oIter == m_oMapAttributes.end())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attribute %s is not an attribute of this group",
                 osName.c_str());
        return false;
    }

    oIter->second->Deleted();
    m_oMapAttributes.erase(oIter);
    return true;
}

/************************************************************************/
/*                              Rename()                                */
/************************************************************************/

bool MEMGroup::Rename(const std::string &osNewName)
{
    if (!CheckValidAndErrorOutIfNot())
        return false;
    if (osNewName.empty())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Empty name not supported");
        return false;
    }
    if (m_osName == "/")
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Cannot rename root group");
        return false;
    }
    auto pParent = m_pParent.lock();
    if (pParent)
    {
        if (pParent->m_oMapGroups.find(osNewName) !=
            pParent->m_oMapGroups.end())
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "A group with same name already exists");
            return false;
        }
        pParent->m_oMapGroups.erase(pParent->m_oMapGroups.find(m_osName));
    }

    BaseRename(osNewName);

    if (pParent)
    {
        CPLAssert(m_pSelf.lock());
        pParent->m_oMapGroups[m_osName] = m_pSelf.lock();
    }

    return true;
}

/************************************************************************/
/*                       NotifyChildrenOfRenaming()                     */
/************************************************************************/

void MEMGroup::NotifyChildrenOfRenaming()
{
    for (const auto &oIter : m_oMapGroups)
        oIter.second->ParentRenamed(m_osFullName);
    for (const auto &oIter : m_oMapMDArrays)
        oIter.second->ParentRenamed(m_osFullName);
    for (const auto &oIter : m_oMapAttributes)
        oIter.second->ParentRenamed(m_osFullName);
    for (const auto &oIter : m_oMapDimensions)
        oIter.second->ParentRenamed(m_osFullName);
}

/************************************************************************/
/*                          RenameDimension()                           */
/************************************************************************/

bool MEMGroup::RenameDimension(const std::string &osOldName,
                               const std::string &osNewName)
{
    if (m_oMapDimensions.find(osNewName) != m_oMapDimensions.end())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "A dimension with same name already exists");
        return false;
    }
    auto oIter = m_oMapDimensions.find(osOldName);
    if (oIter == m_oMapDimensions.end())
    {
        CPLAssert(false);
        return false;
    }
    auto poDim = std::move(oIter->second);
    m_oMapDimensions.erase(oIter);
    m_oMapDimensions[osNewName] = std::move(poDim);
    return true;
}

/************************************************************************/
/*                          RenameArray()                               */
/************************************************************************/

bool MEMGroup::RenameArray(const std::string &osOldName,
                           const std::string &osNewName)
{
    if (m_oMapMDArrays.find(osNewName) != m_oMapMDArrays.end())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "An array with same name already exists");
        return false;
    }
    auto oIter = m_oMapMDArrays.find(osOldName);
    if (oIter == m_oMapMDArrays.end())
    {
        CPLAssert(false);
        return false;
    }
    auto poArray = std::move(oIter->second);
    m_oMapMDArrays.erase(oIter);
    m_oMapMDArrays[osNewName] = std::move(poArray);
    return true;
}

/************************************************************************/
/*                          MEMAbstractMDArray()                        */
/************************************************************************/

MEMAbstractMDArray::MEMAbstractMDArray(
    const std::string &osParentName, const std::string &osName,
    const std::vector<std::shared_ptr<GDALDimension>> &aoDimensions,
    const GDALExtendedDataType &oType)
    : GDALAbstractMDArray(osParentName, osName), m_aoDims(aoDimensions),
      m_oType(oType)
{
}

/************************************************************************/
/*                         ~MEMAbstractMDArray()                        */
/************************************************************************/

MEMAbstractMDArray::~MEMAbstractMDArray()
{
    FreeArray();
}

/************************************************************************/
/*                              FreeArray()                             */
/************************************************************************/

void MEMAbstractMDArray::FreeArray()
{
    if (m_bOwnArray)
    {
        if (m_oType.NeedsFreeDynamicMemory())
        {
            GByte *pabyPtr = m_pabyArray;
            GByte *pabyEnd = m_pabyArray + m_nTotalSize;
            const auto nDTSize(m_oType.GetSize());
            while (pabyPtr < pabyEnd)
            {
                m_oType.FreeDynamicMemory(pabyPtr);
                pabyPtr += nDTSize;
            }
        }
        VSIFree(m_pabyArray);
        m_pabyArray = nullptr;
        m_nTotalSize = 0;
        m_bOwnArray = false;
    }
}

/************************************************************************/
/*                                  Init()                              */
/************************************************************************/

bool MEMAbstractMDArray::Init(GByte *pData,
                              const std::vector<GPtrDiff_t> &anStrides)
{
    GUInt64 nTotalSize = m_oType.GetSize();
    if (!m_aoDims.empty())
    {
        if (anStrides.empty())
        {
            m_anStrides.resize(m_aoDims.size());
        }
        else
        {
            CPLAssert(anStrides.size() == m_aoDims.size());
            m_anStrides = anStrides;
        }

        // To compute strides we must proceed from the fastest varying dimension
        // (the last one), and then reverse the result
        for (size_t i = m_aoDims.size(); i != 0;)
        {
            --i;
            const auto &poDim = m_aoDims[i];
            auto nDimSize = poDim->GetSize();
            if (nDimSize == 0)
            {
                CPLError(CE_Failure, CPLE_IllegalArg,
                         "Illegal dimension size 0");
                return false;
            }
            if (nTotalSize > std::numeric_limits<GUInt64>::max() / nDimSize)
            {
                CPLError(CE_Failure, CPLE_OutOfMemory, "Too big allocation");
                return false;
            }
            auto nNewSize = nTotalSize * nDimSize;
            if (anStrides.empty())
                m_anStrides[i] = static_cast<size_t>(nTotalSize);
            nTotalSize = nNewSize;
        }
    }

    // We restrict the size of the allocation so that all elements can be
    // indexed by GPtrDiff_t
    if (nTotalSize >
        static_cast<size_t>(std::numeric_limits<GPtrDiff_t>::max()))
    {
        CPLError(CE_Failure, CPLE_OutOfMemory, "Too big allocation");
        return false;
    }
    m_nTotalSize = static_cast<size_t>(nTotalSize);
    if (pData)
    {
        m_pabyArray = pData;
    }
    else
    {
        m_pabyArray = static_cast<GByte *>(VSI_CALLOC_VERBOSE(1, m_nTotalSize));
        m_bOwnArray = true;
    }

    return m_pabyArray != nullptr;
}

/************************************************************************/
/*                             FastCopy()                               */
/************************************************************************/

template <int N>
inline static void FastCopy(size_t nIters, GByte *dstPtr, const GByte *srcPtr,
                            GPtrDiff_t dst_inc_offset,
                            GPtrDiff_t src_inc_offset)
{
    if (nIters >= 8)
    {
#define COPY_ELT(i)                                                            \
    memcpy(dstPtr + (i)*dst_inc_offset, srcPtr + (i)*src_inc_offset, N)
        while (true)
        {
            COPY_ELT(0);
            COPY_ELT(1);
            COPY_ELT(2);
            COPY_ELT(3);
            COPY_ELT(4);
            COPY_ELT(5);
            COPY_ELT(6);
            COPY_ELT(7);
            nIters -= 8;
            srcPtr += 8 * src_inc_offset;
            dstPtr += 8 * dst_inc_offset;
            if (nIters < 8)
                break;
        }
        if (nIters == 0)
            return;
    }
    while (true)
    {
        memcpy(dstPtr, srcPtr, N);
        if ((--nIters) == 0)
            break;
        srcPtr += src_inc_offset;
        dstPtr += dst_inc_offset;
    }
}

/************************************************************************/
/*                             ReadWrite()                              */
/************************************************************************/

void MEMAbstractMDArray::ReadWrite(bool bIsWrite, const size_t *count,
                                   std::vector<StackReadWrite> &stack,
                                   const GDALExtendedDataType &srcType,
                                   const GDALExtendedDataType &dstType) const
{
    const auto nDims = m_aoDims.size();
    const auto nDimsMinus1 = nDims - 1;
    const bool bBothAreNumericDT = srcType.GetClass() == GEDTC_NUMERIC &&
                                   dstType.GetClass() == GEDTC_NUMERIC;
    const bool bSameNumericDT =
        bBothAreNumericDT &&
        srcType.GetNumericDataType() == dstType.GetNumericDataType();
    const auto nSameDTSize = bSameNumericDT ? srcType.GetSize() : 0;
    const bool bCanUseMemcpyLastDim =
        bSameNumericDT &&
        stack[nDimsMinus1].src_inc_offset ==
            static_cast<GPtrDiff_t>(nSameDTSize) &&
        stack[nDimsMinus1].dst_inc_offset ==
            static_cast<GPtrDiff_t>(nSameDTSize);
    const size_t nCopySizeLastDim =
        bCanUseMemcpyLastDim ? nSameDTSize * count[nDimsMinus1] : 0;
    const bool bNeedsFreeDynamicMemory =
        bIsWrite && dstType.NeedsFreeDynamicMemory();

    auto lambdaLastDim = [&](size_t idxPtr)
    {
        auto srcPtr = stack[idxPtr].src_ptr;
        auto dstPtr = stack[idxPtr].dst_ptr;
        if (nCopySizeLastDim)
        {
            memcpy(dstPtr, srcPtr, nCopySizeLastDim);
        }
        else
        {
            size_t nIters = count[nDimsMinus1];
            const auto dst_inc_offset = stack[nDimsMinus1].dst_inc_offset;
            const auto src_inc_offset = stack[nDimsMinus1].src_inc_offset;
            if (bSameNumericDT)
            {
                if (nSameDTSize == 1)
                {
                    FastCopy<1>(nIters, dstPtr, srcPtr, dst_inc_offset,
                                src_inc_offset);
                    return;
                }
                if (nSameDTSize == 2)
                {
                    FastCopy<2>(nIters, dstPtr, srcPtr, dst_inc_offset,
                                src_inc_offset);
                    return;
                }
                if (nSameDTSize == 4)
                {
                    FastCopy<4>(nIters, dstPtr, srcPtr, dst_inc_offset,
                                src_inc_offset);
                    return;
                }
                if (nSameDTSize == 8)
                {
                    FastCopy<8>(nIters, dstPtr, srcPtr, dst_inc_offset,
                                src_inc_offset);
                    return;
                }
                if (nSameDTSize == 16)
                {
                    FastCopy<16>(nIters, dstPtr, srcPtr, dst_inc_offset,
                                 src_inc_offset);
                    return;
                }
                CPLAssert(false);
            }
            else if (bBothAreNumericDT
#if SIZEOF_VOIDP >= 8
                     && src_inc_offset <= std::numeric_limits<int>::max() &&
                     dst_inc_offset <= std::numeric_limits<int>::max()
#endif
            )
            {
                GDALCopyWords64(srcPtr, srcType.GetNumericDataType(),
                                static_cast<int>(src_inc_offset), dstPtr,
                                dstType.GetNumericDataType(),
                                static_cast<int>(dst_inc_offset),
                                static_cast<GPtrDiff_t>(nIters));
                return;
            }

            while (true)
            {
                if (bNeedsFreeDynamicMemory)
                {
                    dstType.FreeDynamicMemory(dstPtr);
                }
                GDALExtendedDataType::CopyValue(srcPtr, srcType, dstPtr,
                                                dstType);
                if ((--nIters) == 0)
                    break;
                srcPtr += src_inc_offset;
                dstPtr += dst_inc_offset;
            }
        }
    };

    if (nDims == 1)
    {
        lambdaLastDim(0);
    }
    else if (nDims == 2)
    {
        auto nIters = count[0];
        while (true)
        {
            lambdaLastDim(0);
            if ((--nIters) == 0)
                break;
            stack[0].src_ptr += stack[0].src_inc_offset;
            stack[0].dst_ptr += stack[0].dst_inc_offset;
        }
    }
    else if (nDims == 3)
    {
        stack[0].nIters = count[0];
        while (true)
        {
            stack[1].src_ptr = stack[0].src_ptr;
            stack[1].dst_ptr = stack[0].dst_ptr;
            auto nIters = count[1];
            while (true)
            {
                lambdaLastDim(1);
                if ((--nIters) == 0)
                    break;
                stack[1].src_ptr += stack[1].src_inc_offset;
                stack[1].dst_ptr += stack[1].dst_inc_offset;
            }
            if ((--stack[0].nIters) == 0)
                break;
            stack[0].src_ptr += stack[0].src_inc_offset;
            stack[0].dst_ptr += stack[0].dst_inc_offset;
        }
    }
    else
    {
        // Implementation valid for nDims >= 3

        size_t dimIdx = 0;
        // Non-recursive implementation. Hence the gotos
        // It might be possible to rewrite this without gotos, but I find they
        // make it clearer to understand the recursive nature of the code
    lbl_next_depth:
        if (dimIdx == nDimsMinus1 - 1)
        {
            auto nIters = count[dimIdx];
            while (true)
            {
                lambdaLastDim(dimIdx);
                if ((--nIters) == 0)
                    break;
                stack[dimIdx].src_ptr += stack[dimIdx].src_inc_offset;
                stack[dimIdx].dst_ptr += stack[dimIdx].dst_inc_offset;
            }
            // If there was a test if( dimIdx > 0 ), that would be valid for
            // nDims == 2
            goto lbl_return_to_caller;
        }
        else
        {
            stack[dimIdx].nIters = count[dimIdx];
            while (true)
            {
                dimIdx++;
                stack[dimIdx].src_ptr = stack[dimIdx - 1].src_ptr;
                stack[dimIdx].dst_ptr = stack[dimIdx - 1].dst_ptr;
                goto lbl_next_depth;
            lbl_return_to_caller:
                dimIdx--;
                if ((--stack[dimIdx].nIters) == 0)
                    break;
                stack[dimIdx].src_ptr += stack[dimIdx].src_inc_offset;
                stack[dimIdx].dst_ptr += stack[dimIdx].dst_inc_offset;
            }
            if (dimIdx > 0)
                goto lbl_return_to_caller;
        }
    }
}

/************************************************************************/
/*                                   IRead()                            */
/************************************************************************/

bool MEMAbstractMDArray::IRead(const GUInt64 *arrayStartIdx,
                               const size_t *count, const GInt64 *arrayStep,
                               const GPtrDiff_t *bufferStride,
                               const GDALExtendedDataType &bufferDataType,
                               void *pDstBuffer) const
{
    if (!CheckValidAndErrorOutIfNot())
        return false;

    const auto nDims = m_aoDims.size();
    if (nDims == 0)
    {
        GDALExtendedDataType::CopyValue(m_pabyArray, m_oType, pDstBuffer,
                                        bufferDataType);
        return true;
    }
    std::vector<StackReadWrite> stack(nDims);
    const auto nBufferDTSize = bufferDataType.GetSize();
    GPtrDiff_t startSrcOffset = 0;
    for (size_t i = 0; i < nDims; i++)
    {
        startSrcOffset +=
            static_cast<GPtrDiff_t>(arrayStartIdx[i] * m_anStrides[i]);
        stack[i].src_inc_offset =
            static_cast<GPtrDiff_t>(arrayStep[i] * m_anStrides[i]);
        stack[i].dst_inc_offset =
            static_cast<GPtrDiff_t>(bufferStride[i] * nBufferDTSize);
    }
    stack[0].src_ptr = m_pabyArray + startSrcOffset;
    stack[0].dst_ptr = static_cast<GByte *>(pDstBuffer);

    ReadWrite(false, count, stack, m_oType, bufferDataType);
    return true;
}

/************************************************************************/
/*                                IWrite()                              */
/************************************************************************/

bool MEMAbstractMDArray::IWrite(const GUInt64 *arrayStartIdx,
                                const size_t *count, const GInt64 *arrayStep,
                                const GPtrDiff_t *bufferStride,
                                const GDALExtendedDataType &bufferDataType,
                                const void *pSrcBuffer)
{
    if (!CheckValidAndErrorOutIfNot())
        return false;
    if (!m_bWritable)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Non updatable object");
        return false;
    }

    m_bModified = true;

    const auto nDims = m_aoDims.size();
    if (nDims == 0)
    {
        m_oType.FreeDynamicMemory(m_pabyArray);
        GDALExtendedDataType::CopyValue(pSrcBuffer, bufferDataType, m_pabyArray,
                                        m_oType);
        return true;
    }
    std::vector<StackReadWrite> stack(nDims);
    const auto nBufferDTSize = bufferDataType.GetSize();
    GPtrDiff_t startDstOffset = 0;
    for (size_t i = 0; i < nDims; i++)
    {
        startDstOffset +=
            static_cast<GPtrDiff_t>(arrayStartIdx[i] * m_anStrides[i]);
        stack[i].dst_inc_offset =
            static_cast<GPtrDiff_t>(arrayStep[i] * m_anStrides[i]);
        stack[i].src_inc_offset =
            static_cast<GPtrDiff_t>(bufferStride[i] * nBufferDTSize);
    }

    stack[0].dst_ptr = m_pabyArray + startDstOffset;
    stack[0].src_ptr = static_cast<const GByte *>(pSrcBuffer);

    ReadWrite(true, count, stack, bufferDataType, m_oType);
    return true;
}

/************************************************************************/
/*                               MEMMDArray()                           */
/************************************************************************/

MEMMDArray::MEMMDArray(
    const std::string &osParentName, const std::string &osName,
    const std::vector<std::shared_ptr<GDALDimension>> &aoDimensions,
    const GDALExtendedDataType &oType)
    : GDALAbstractMDArray(osParentName, osName),
      MEMAbstractMDArray(osParentName, osName, aoDimensions, oType),
      GDALMDArray(osParentName, osName)
{
}

/************************************************************************/
/*                              ~MEMMDArray()                           */
/************************************************************************/

MEMMDArray::~MEMMDArray()
{
    if (m_pabyNoData)
    {
        m_oType.FreeDynamicMemory(&m_pabyNoData[0]);
        CPLFree(m_pabyNoData);
    }

    for (auto &poDim : GetDimensions())
    {
        const auto dim = std::dynamic_pointer_cast<MEMDimension>(poDim);
        if (dim)
            dim->UnRegisterUsingArray(this);
    }
}

/************************************************************************/
/*                          GetRawNoDataValue()                         */
/************************************************************************/

const void *MEMMDArray::GetRawNoDataValue() const
{
    return m_pabyNoData;
}

/************************************************************************/
/*                          SetRawNoDataValue()                         */
/************************************************************************/

bool MEMMDArray::SetRawNoDataValue(const void *pNoData)
{
    if (!CheckValidAndErrorOutIfNot())
        return false;
    if (m_pabyNoData)
    {
        m_oType.FreeDynamicMemory(&m_pabyNoData[0]);
    }

    if (pNoData == nullptr)
    {
        CPLFree(m_pabyNoData);
        m_pabyNoData = nullptr;
    }
    else
    {
        const auto nSize = m_oType.GetSize();
        if (m_pabyNoData == nullptr)
        {
            m_pabyNoData = static_cast<GByte *>(CPLMalloc(nSize));
        }
        memset(m_pabyNoData, 0, nSize);
        GDALExtendedDataType::CopyValue(pNoData, m_oType, m_pabyNoData,
                                        m_oType);
    }
    return true;
}

/************************************************************************/
/*                            GetAttribute()                            */
/************************************************************************/

std::shared_ptr<GDALAttribute>
MEMMDArray::GetAttribute(const std::string &osName) const
{
    if (!CheckValidAndErrorOutIfNot())
        return nullptr;
    auto oIter = m_oMapAttributes.find(osName);
    if (oIter != m_oMapAttributes.end())
        return oIter->second;
    return nullptr;
}

/************************************************************************/
/*                             GetAttributes()                          */
/************************************************************************/

std::vector<std::shared_ptr<GDALAttribute>>
MEMMDArray::GetAttributes(CSLConstList) const
{
    if (!CheckValidAndErrorOutIfNot())
        return {};
    std::vector<std::shared_ptr<GDALAttribute>> oRes;
    for (const auto &oIter : m_oMapAttributes)
    {
        oRes.push_back(oIter.second);
    }
    return oRes;
}

/************************************************************************/
/*                            CreateAttribute()                         */
/************************************************************************/

std::shared_ptr<GDALAttribute>
MEMMDArray::CreateAttribute(const std::string &osName,
                            const std::vector<GUInt64> &anDimensions,
                            const GDALExtendedDataType &oDataType, CSLConstList)
{
    if (!CheckValidAndErrorOutIfNot())
        return nullptr;
    if (osName.empty())
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Empty attribute name not supported");
        return nullptr;
    }
    if (m_oMapAttributes.find(osName) != m_oMapAttributes.end())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "An attribute with same name already exists");
        return nullptr;
    }
    auto poSelf = std::dynamic_pointer_cast<MEMMDArray>(m_pSelf.lock());
    CPLAssert(poSelf);
    auto newAttr(MEMAttribute::Create(poSelf, osName, anDimensions, oDataType));
    if (!newAttr)
        return nullptr;
    m_oMapAttributes[osName] = newAttr;
    return newAttr;
}

/************************************************************************/
/*                         DeleteAttribute()                            */
/************************************************************************/

bool MEMMDArray::DeleteAttribute(const std::string &osName,
                                 CSLConstList /*papszOptions*/)
{
    if (!CheckValidAndErrorOutIfNot())
        return false;
    auto oIter = m_oMapAttributes.find(osName);
    if (oIter == m_oMapAttributes.end())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attribute %s is not an attribute of this array",
                 osName.c_str());
        return false;
    }

    oIter->second->Deleted();
    m_oMapAttributes.erase(oIter);
    return true;
}

/************************************************************************/
/*                      GetCoordinateVariables()                        */
/************************************************************************/

std::vector<std::shared_ptr<GDALMDArray>>
MEMMDArray::GetCoordinateVariables() const
{
    if (!CheckValidAndErrorOutIfNot())
        return {};
    std::vector<std::shared_ptr<GDALMDArray>> ret;
    const auto poCoordinates = GetAttribute("coordinates");
    if (poCoordinates &&
        poCoordinates->GetDataType().GetClass() == GEDTC_STRING &&
        poCoordinates->GetDimensionCount() == 0)
    {
        const char *pszCoordinates = poCoordinates->ReadAsString();
        if (pszCoordinates)
        {
            auto poGroup = m_poGroupWeak.lock();
            if (!poGroup)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Cannot access coordinate variables of %s has "
                         "belonging group has gone out of scope",
                         GetName().c_str());
            }
            else
            {
                const CPLStringList aosNames(
                    CSLTokenizeString2(pszCoordinates, " ", 0));
                for (int i = 0; i < aosNames.size(); i++)
                {
                    auto poCoordinateVar = poGroup->OpenMDArray(aosNames[i]);
                    if (poCoordinateVar)
                    {
                        ret.emplace_back(poCoordinateVar);
                    }
                    else
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "Cannot find variable corresponding to "
                                 "coordinate %s",
                                 aosNames[i]);
                    }
                }
            }
        }
    }

    return ret;
}

/************************************************************************/
/*                            Resize()                                  */
/************************************************************************/

bool MEMMDArray::Resize(const std::vector<GUInt64> &anNewDimSizes,
                        CSLConstList /* papszOptions */)
{
    return Resize(anNewDimSizes, /*bResizeOtherArrays=*/true);
}

bool MEMMDArray::Resize(const std::vector<GUInt64> &anNewDimSizes,
                        bool bResizeOtherArrays)
{
    if (!CheckValidAndErrorOutIfNot())
        return false;
    if (!IsWritable())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Resize() not supported on read-only file");
        return false;
    }
    if (!m_bOwnArray)
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "Resize() not supported on an array that does not own its memory");
        return false;
    }

    const auto nDimCount = GetDimensionCount();
    if (anNewDimSizes.size() != nDimCount)
    {
        CPLError(CE_Failure, CPLE_IllegalArg,
                 "Not expected number of values in anNewDimSizes.");
        return false;
    }

    auto &dims = GetDimensions();
    std::vector<size_t> anDecreasedDimIdx;
    std::vector<size_t> anGrownDimIdx;
    std::map<GDALDimension *, GUInt64> oMapDimToSize;
    for (size_t i = 0; i < nDimCount; ++i)
    {
        auto oIter = oMapDimToSize.find(dims[i].get());
        if (oIter != oMapDimToSize.end() && oIter->second != anNewDimSizes[i])
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot resize a dimension referenced several times "
                     "to different sizes");
            return false;
        }
        if (anNewDimSizes[i] != dims[i]->GetSize())
        {
            if (anNewDimSizes[i] == 0)
            {
                CPLError(CE_Failure, CPLE_IllegalArg,
                         "Illegal dimension size 0");
                return false;
            }
            auto dim = std::dynamic_pointer_cast<MEMDimension>(dims[i]);
            if (!dim)
            {
                CPLError(
                    CE_Failure, CPLE_AppDefined,
                    "Cannot resize a dimension that is not a MEMDimension");
                return false;
            }
            oMapDimToSize[dim.get()] = anNewDimSizes[i];
            if (anNewDimSizes[i] < dims[i]->GetSize())
            {
                anDecreasedDimIdx.push_back(i);
            }
            else
            {
                anGrownDimIdx.push_back(i);
            }
        }
        else
        {
            oMapDimToSize[dims[i].get()] = dims[i]->GetSize();
        }
    }

    const auto ResizeOtherArrays = [this, &anNewDimSizes, nDimCount, &dims]()
    {
        std::set<MEMMDArray *> oSetArrays;
        std::map<GDALDimension *, GUInt64> oMapNewSize;
        for (size_t i = 0; i < nDimCount; ++i)
        {
            if (anNewDimSizes[i] != dims[i]->GetSize())
            {
                auto dim = std::dynamic_pointer_cast<MEMDimension>(dims[i]);
                if (!dim)
                {
                    CPLAssert(false);
                }
                else
                {
                    oMapNewSize[dims[i].get()] = anNewDimSizes[i];
                    for (const auto &poArray : dim->GetUsingArrays())
                    {
                        if (poArray != this)
                            oSetArrays.insert(poArray);
                    }
                }
            }
        }

        bool bOK = true;
        for (auto *poArray : oSetArrays)
        {
            const auto &apoOtherDims = poArray->GetDimensions();
            std::vector<GUInt64> anOtherArrayNewDimSizes(
                poArray->GetDimensionCount());
            for (size_t i = 0; i < anOtherArrayNewDimSizes.size(); ++i)
            {
                auto oIter = oMapNewSize.find(apoOtherDims[i].get());
                if (oIter != oMapNewSize.end())
                    anOtherArrayNewDimSizes[i] = oIter->second;
                else
                    anOtherArrayNewDimSizes[i] = apoOtherDims[i]->GetSize();
            }
            if (!poArray->Resize(anOtherArrayNewDimSizes,
                                 /*bResizeOtherArrays=*/false))
            {
                bOK = false;
                break;
            }
        }
        if (!bOK)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Resizing of another array referencing the same dimension "
                     "as one modified on the current array failed. All arrays "
                     "referencing that dimension will be invalidated.");
            Invalidate();
            for (auto *poArray : oSetArrays)
            {
                poArray->Invalidate();
            }
        }

        return bOK;
    };

    // Decrease slowest varying dimension
    if (anGrownDimIdx.empty() && anDecreasedDimIdx.size() == 1 &&
        anDecreasedDimIdx[0] == 0)
    {
        CPLAssert(m_nTotalSize % dims[0]->GetSize() == 0);
        const size_t nNewTotalSize = static_cast<size_t>(
            (m_nTotalSize / dims[0]->GetSize()) * anNewDimSizes[0]);
        if (m_oType.NeedsFreeDynamicMemory())
        {
            GByte *pabyPtr = m_pabyArray + nNewTotalSize;
            GByte *pabyEnd = m_pabyArray + m_nTotalSize;
            const auto nDTSize(m_oType.GetSize());
            while (pabyPtr < pabyEnd)
            {
                m_oType.FreeDynamicMemory(pabyPtr);
                pabyPtr += nDTSize;
            }
        }
        // shrinking... cannot fail, and even if it does, that's ok
        GByte *pabyArray = static_cast<GByte *>(
            VSI_REALLOC_VERBOSE(m_pabyArray, nNewTotalSize));
        if (pabyArray)
            m_pabyArray = pabyArray;
        m_nTotalSize = nNewTotalSize;

        if (bResizeOtherArrays)
        {
            if (!ResizeOtherArrays())
                return false;

            auto dim = std::dynamic_pointer_cast<MEMDimension>(dims[0]);
            if (dim)
            {
                dim->SetSize(anNewDimSizes[0]);
            }
            else
            {
                CPLAssert(false);
            }
        }
        return true;
    }

    // Increase slowest varying dimension
    if (anDecreasedDimIdx.empty() && anGrownDimIdx.size() == 1 &&
        anGrownDimIdx[0] == 0)
    {
        CPLAssert(m_nTotalSize % dims[0]->GetSize() == 0);
        GUInt64 nNewTotalSize64 = m_nTotalSize / dims[0]->GetSize();
        if (nNewTotalSize64 >
            std::numeric_limits<GUInt64>::max() / anNewDimSizes[0])
        {
            CPLError(CE_Failure, CPLE_OutOfMemory, "Too big allocation");
            return false;
        }
        nNewTotalSize64 *= anNewDimSizes[0];
        // We restrict the size of the allocation so that all elements can be
        // indexed by GPtrDiff_t
        if (nNewTotalSize64 >
            static_cast<size_t>(std::numeric_limits<GPtrDiff_t>::max()))
        {
            CPLError(CE_Failure, CPLE_OutOfMemory, "Too big allocation");
            return false;
        }
        const size_t nNewTotalSize = static_cast<size_t>(nNewTotalSize64);
        GByte *pabyArray = static_cast<GByte *>(
            VSI_REALLOC_VERBOSE(m_pabyArray, nNewTotalSize));
        if (!pabyArray)
            return false;
        memset(pabyArray + m_nTotalSize, 0, nNewTotalSize - m_nTotalSize);
        m_pabyArray = pabyArray;
        m_nTotalSize = nNewTotalSize;

        if (bResizeOtherArrays)
        {
            if (!ResizeOtherArrays())
                return false;

            auto dim = std::dynamic_pointer_cast<MEMDimension>(dims[0]);
            if (dim)
            {
                dim->SetSize(anNewDimSizes[0]);
            }
            else
            {
                CPLAssert(false);
            }
        }
        return true;
    }

    // General case where we modify other dimensions that the first one.

    // Create dummy dimensions at the new sizes
    std::vector<std::shared_ptr<GDALDimension>> aoNewDims;
    for (size_t i = 0; i < nDimCount; ++i)
    {
        aoNewDims.emplace_back(std::make_shared<MEMDimension>(
            std::string(), dims[i]->GetName(), std::string(), std::string(),
            anNewDimSizes[i]));
    }

    // Create a temporary array
    auto poTempMDArray =
        Create(std::string(), std::string(), aoNewDims, GetDataType());
    if (!poTempMDArray->Init())
        return false;
    std::vector<GUInt64> arrayStartIdx(nDimCount);
    std::vector<size_t> count(nDimCount);
    std::vector<GInt64> arrayStep(nDimCount, 1);
    std::vector<GPtrDiff_t> bufferStride(nDimCount);
    for (size_t i = nDimCount; i > 0;)
    {
        --i;
        if (i == nDimCount - 1)
            bufferStride[i] = 1;
        else
        {
            bufferStride[i] = static_cast<GPtrDiff_t>(bufferStride[i + 1] *
                                                      dims[i + 1]->GetSize());
        }
        const auto nCount = std::min(anNewDimSizes[i], dims[i]->GetSize());
        count[i] = static_cast<size_t>(nCount);
    }
    // Copy the current content into the array with the new layout
    if (!poTempMDArray->Write(arrayStartIdx.data(), count.data(),
                              arrayStep.data(), bufferStride.data(),
                              GetDataType(), m_pabyArray))
    {
        return false;
    }

    // Move content of the temporary array into the current array, and
    // invalidate the temporary array
    FreeArray();
    m_bOwnArray = true;
    m_pabyArray = poTempMDArray->m_pabyArray;
    m_nTotalSize = poTempMDArray->m_nTotalSize;
    m_anStrides = poTempMDArray->m_anStrides;

    poTempMDArray->m_bOwnArray = false;
    poTempMDArray->m_pabyArray = nullptr;
    poTempMDArray->m_nTotalSize = 0;

    if (bResizeOtherArrays && !ResizeOtherArrays())
        return false;

    // Update dimension size
    for (size_t i = 0; i < nDimCount; ++i)
    {
        if (anNewDimSizes[i] != dims[i]->GetSize())
        {
            auto dim = std::dynamic_pointer_cast<MEMDimension>(dims[i]);
            if (dim)
            {
                dim->SetSize(anNewDimSizes[i]);
            }
            else
            {
                CPLAssert(false);
            }
        }
    }

    return true;
}

/************************************************************************/
/*                              Rename()                                */
/************************************************************************/

bool MEMMDArray::Rename(const std::string &osNewName)
{
    if (!CheckValidAndErrorOutIfNot())
        return false;
    if (osNewName.empty())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Empty name not supported");
        return false;
    }

    if (auto poParentGroup =
            std::dynamic_pointer_cast<MEMGroup>(m_poGroupWeak.lock()))
    {
        if (!poParentGroup->RenameArray(m_osName, osNewName))
        {
            return false;
        }
    }

    BaseRename(osNewName);

    return true;
}

/************************************************************************/
/*                       NotifyChildrenOfRenaming()                     */
/************************************************************************/

void MEMMDArray::NotifyChildrenOfRenaming()
{
    for (const auto &oIter : m_oMapAttributes)
        oIter.second->ParentRenamed(m_osFullName);
}

/************************************************************************/
/*                       NotifyChildrenOfDeletion()                     */
/************************************************************************/

void MEMMDArray::NotifyChildrenOfDeletion()
{
    for (const auto &oIter : m_oMapAttributes)
        oIter.second->ParentDeleted();
}

/************************************************************************/
/*                            BuildDimensions()                         */
/************************************************************************/

static std::vector<std::shared_ptr<GDALDimension>>
BuildDimensions(const std::vector<GUInt64> &anDimensions)
{
    std::vector<std::shared_ptr<GDALDimension>> res;
    for (size_t i = 0; i < anDimensions.size(); i++)
    {
        res.emplace_back(std::make_shared<GDALDimensionWeakIndexingVar>(
            std::string(), CPLSPrintf("dim%u", static_cast<unsigned>(i)),
            std::string(), std::string(), anDimensions[i]));
    }
    return res;
}

/************************************************************************/
/*                             MEMAttribute()                           */
/************************************************************************/

MEMAttribute::MEMAttribute(const std::string &osParentName,
                           const std::string &osName,
                           const std::vector<GUInt64> &anDimensions,
                           const GDALExtendedDataType &oType)
    : GDALAbstractMDArray(osParentName, osName),
      MEMAbstractMDArray(osParentName, osName, BuildDimensions(anDimensions),
                         oType),
      GDALAttribute(osParentName, osName)
{
}

/************************************************************************/
/*                        MEMAttribute::Create()                        */
/************************************************************************/

std::shared_ptr<MEMAttribute>
MEMAttribute::Create(const std::string &osParentName, const std::string &osName,
                     const std::vector<GUInt64> &anDimensions,
                     const GDALExtendedDataType &oType)
{
    auto attr(std::shared_ptr<MEMAttribute>(
        new MEMAttribute(osParentName, osName, anDimensions, oType)));
    attr->SetSelf(attr);
    if (!attr->Init())
        return nullptr;
    return attr;
}

/************************************************************************/
/*                        MEMAttribute::Create()                        */
/************************************************************************/

std::shared_ptr<MEMAttribute> MEMAttribute::Create(
    const std::shared_ptr<MEMGroup> &poParentGroup, const std::string &osName,
    const std::vector<GUInt64> &anDimensions, const GDALExtendedDataType &oType)
{
    const std::string osParentName =
        (poParentGroup && poParentGroup->GetName().empty())
            ?
            // Case of the ZarrAttributeGroup::m_oGroup fake group
            poParentGroup->GetFullName()
            : ((poParentGroup == nullptr || poParentGroup->GetFullName() == "/"
                    ? "/"
                    : poParentGroup->GetFullName() + "/") +
               "_GLOBAL_");
    auto attr(Create(osParentName, osName, anDimensions, oType));
    if (!attr)
        return nullptr;
    attr->m_poParent = poParentGroup;
    return attr;
}

/************************************************************************/
/*                        MEMAttribute::Create()                        */
/************************************************************************/

std::shared_ptr<MEMAttribute> MEMAttribute::Create(
    const std::shared_ptr<MEMMDArray> &poParentArray, const std::string &osName,
    const std::vector<GUInt64> &anDimensions, const GDALExtendedDataType &oType)
{
    auto attr(
        Create(poParentArray->GetFullName(), osName, anDimensions, oType));
    if (!attr)
        return nullptr;
    attr->m_poParent = poParentArray;
    return attr;
}

/************************************************************************/
/*                              Rename()                                */
/************************************************************************/

bool MEMAttribute::Rename(const std::string &osNewName)
{
    if (!CheckValidAndErrorOutIfNot())
        return false;
    if (osNewName.empty())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Empty name not supported");
        return false;
    }

    if (auto poParent = m_poParent.lock())
    {
        if (!poParent->RenameAttribute(m_osName, osNewName))
        {
            return false;
        }
    }

    BaseRename(osNewName);

    m_bModified = true;

    return true;
}

/************************************************************************/
/*                             MEMDimension()                           */
/************************************************************************/

MEMDimension::MEMDimension(const std::string &osParentName,
                           const std::string &osName, const std::string &osType,
                           const std::string &osDirection, GUInt64 nSize)
    : GDALDimensionWeakIndexingVar(osParentName, osName, osType, osDirection,
                                   nSize)
{
}

/************************************************************************/
/*                        RegisterUsingArray()                          */
/************************************************************************/

void MEMDimension::RegisterUsingArray(MEMMDArray *poArray)
{
    m_oSetArrays.insert(poArray);
}

/************************************************************************/
/*                        UnRegisterUsingArray()                        */
/************************************************************************/

void MEMDimension::UnRegisterUsingArray(MEMMDArray *poArray)
{
    m_oSetArrays.erase(poArray);
}

/************************************************************************/
/*                                Create()                              */
/************************************************************************/

/* static */
std::shared_ptr<MEMDimension>
MEMDimension::Create(const std::shared_ptr<MEMGroup> &poParentGroup,
                     const std::string &osName, const std::string &osType,
                     const std::string &osDirection, GUInt64 nSize)
{
    auto newDim(std::make_shared<MEMDimension>(
        poParentGroup->GetFullName(), osName, osType, osDirection, nSize));
    newDim->m_poParentGroup = poParentGroup;
    return newDim;
}

/************************************************************************/
/*                             CreateDimension()                        */
/************************************************************************/

std::shared_ptr<GDALDimension>
MEMGroup::CreateDimension(const std::string &osName, const std::string &osType,
                          const std::string &osDirection, GUInt64 nSize,
                          CSLConstList)
{
    if (osName.empty())
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Empty dimension name not supported");
        return nullptr;
    }
    if (m_oMapDimensions.find(osName) != m_oMapDimensions.end())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "A dimension with same name already exists");
        return nullptr;
    }
    auto newDim(MEMDimension::Create(
        std::dynamic_pointer_cast<MEMGroup>(m_pSelf.lock()), osName, osType,
        osDirection, nSize));
    m_oMapDimensions[osName] = newDim;
    return newDim;
}

/************************************************************************/
/*                              Rename()                                */
/************************************************************************/

bool MEMDimension::Rename(const std::string &osNewName)
{
    if (osNewName.empty())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Empty name not supported");
        return false;
    }

    if (auto poParentGroup = m_poParentGroup.lock())
    {
        if (!poParentGroup->RenameDimension(m_osName, osNewName))
        {
            return false;
        }
    }

    BaseRename(osNewName);

    return true;
}

/************************************************************************/
/*                     CreateMultiDimensional()                         */
/************************************************************************/

GDALDataset *
MEMDataset::CreateMultiDimensional(const char *pszFilename,
                                   CSLConstList /*papszRootGroupOptions*/,
                                   CSLConstList /*papszOptions*/)
{
    auto poDS = new MEMDataset();

    poDS->SetDescription(pszFilename);
    auto poRootGroup = MEMGroup::Create(std::string(), nullptr);
    poDS->m_poPrivate->m_poRootGroup = poRootGroup;

    return poDS;
}

/************************************************************************/
/*                          GetRootGroup()                              */
/************************************************************************/

std::shared_ptr<GDALGroup> MEMDataset::GetRootGroup() const
{
    return m_poPrivate->m_poRootGroup;
}

/************************************************************************/
/*                     MEMDatasetIdentify()                             */
/************************************************************************/

static int MEMDatasetIdentify(GDALOpenInfo *poOpenInfo)
{
    return (STARTS_WITH(poOpenInfo->pszFilename, "MEM:::") &&
            poOpenInfo->fpL == nullptr);
}

/************************************************************************/
/*                       MEMDatasetDelete()                             */
/************************************************************************/

static CPLErr MEMDatasetDelete(const char * /* fileName */)
{
    /* Null implementation, so that people can Delete("MEM:::") */
    return CE_None;
}

/************************************************************************/
/*                           ICreateLayer()                             */
/************************************************************************/

OGRLayer *MEMDataset::ICreateLayer(const char *pszLayerName,
                                   const OGRGeomFieldDefn *poGeomFieldDefn,
                                   CSLConstList papszOptions)
{
    // Create the layer object.

    const auto eType = poGeomFieldDefn ? poGeomFieldDefn->GetType() : wkbNone;
    const auto poSRSIn =
        poGeomFieldDefn ? poGeomFieldDefn->GetSpatialRef() : nullptr;

    OGRSpatialReference *poSRS = nullptr;
    if (poSRSIn)
    {
        poSRS = poSRSIn->Clone();
        poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }
    auto poLayer = std::make_unique<OGRMemLayer>(pszLayerName, poSRS, eType);
    if (poSRS)
    {
        poSRS->Release();
    }

    if (CPLFetchBool(papszOptions, "ADVERTIZE_UTF8", false))
        poLayer->SetAdvertizeUTF8(true);

    poLayer->SetDataset(this);
    poLayer->SetFIDColumn(CSLFetchNameValueDef(papszOptions, "FID", ""));

    // Add layer to data source layer list.
    m_apoLayers.emplace_back(std::move(poLayer));
    return m_apoLayers.back().get();
}

/************************************************************************/
/*                            DeleteLayer()                             */
/************************************************************************/

OGRErr MEMDataset::DeleteLayer(int iLayer)

{
    if (iLayer >= 0 && iLayer < static_cast<int>(m_apoLayers.size()))
    {
        m_apoLayers.erase(m_apoLayers.begin() + iLayer);
        return OGRERR_NONE;
    }

    return OGRERR_FAILURE;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int MEMDataset::TestCapability(const char *pszCap)

{
    if (EQUAL(pszCap, ODsCCreateLayer))
        return TRUE;
    else if (EQUAL(pszCap, ODsCDeleteLayer))
        return TRUE;
    else if (EQUAL(pszCap, ODsCCreateGeomFieldAfterCreateLayer))
        return TRUE;
    else if (EQUAL(pszCap, ODsCCurveGeometries))
        return TRUE;
    else if (EQUAL(pszCap, ODsCMeasuredGeometries))
        return TRUE;
    else if (EQUAL(pszCap, ODsCZGeometries))
        return TRUE;
    else if (EQUAL(pszCap, ODsCRandomLayerWrite))
        return TRUE;
    else if (EQUAL(pszCap, ODsCAddFieldDomain))
        return TRUE;
    else if (EQUAL(pszCap, ODsCDeleteFieldDomain))
        return TRUE;
    else if (EQUAL(pszCap, ODsCUpdateFieldDomain))
        return TRUE;

    return FALSE;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *MEMDataset::GetLayer(int iLayer)

{
    if (iLayer < 0 || iLayer >= static_cast<int>(m_apoLayers.size()))
        return nullptr;

    return m_apoLayers[iLayer].get();
}

/************************************************************************/
/*                           AddFieldDomain()                           */
/************************************************************************/

bool MEMDataset::AddFieldDomain(std::unique_ptr<OGRFieldDomain> &&domain,
                                std::string &failureReason)
{
    if (GetFieldDomain(domain->GetName()) != nullptr)
    {
        failureReason = "A domain of identical name already exists";
        return false;
    }
    const std::string domainName(domain->GetName());
    m_oMapFieldDomains[domainName] = std::move(domain);
    return true;
}

/************************************************************************/
/*                           DeleteFieldDomain()                        */
/************************************************************************/

bool MEMDataset::DeleteFieldDomain(const std::string &name,
                                   std::string &failureReason)
{
    const auto iter = m_oMapFieldDomains.find(name);
    if (iter == m_oMapFieldDomains.end())
    {
        failureReason = "Domain does not exist";
        return false;
    }

    m_oMapFieldDomains.erase(iter);

    for (auto &poLayer : m_apoLayers)
    {
        for (int j = 0; j < poLayer->GetLayerDefn()->GetFieldCount(); ++j)
        {
            OGRFieldDefn *poFieldDefn =
                poLayer->GetLayerDefn()->GetFieldDefn(j);
            if (poFieldDefn->GetDomainName() == name)
            {
                auto oTemporaryUnsealer(poFieldDefn->GetTemporaryUnsealer());
                poFieldDefn->SetDomainName(std::string());
            }
        }
    }

    return true;
}

/************************************************************************/
/*                           UpdateFieldDomain()                        */
/************************************************************************/

bool MEMDataset::UpdateFieldDomain(std::unique_ptr<OGRFieldDomain> &&domain,
                                   std::string &failureReason)
{
    const std::string domainName(domain->GetName());
    const auto iter = m_oMapFieldDomains.find(domainName);
    if (iter == m_oMapFieldDomains.end())
    {
        failureReason = "No matching domain found";
        return false;
    }
    m_oMapFieldDomains[domainName] = std::move(domain);
    return true;
}

/************************************************************************/
/*                              ExecuteSQL()                            */
/************************************************************************/

OGRLayer *MEMDataset::ExecuteSQL(const char *pszStatement,
                                 OGRGeometry *poSpatialFilter,
                                 const char *pszDialect)
{
    if (EQUAL(pszStatement, "PRAGMA read_only=1"))  // as used by VDV driver
    {
        for (auto &poLayer : m_apoLayers)
            poLayer->SetUpdatable(false);
        return nullptr;
    }
    return GDALDataset::ExecuteSQL(pszStatement, poSpatialFilter, pszDialect);
}

/************************************************************************/
/*                          GDALRegister_MEM()                          */
/************************************************************************/

void GDALRegister_MEM()
{
    if (GDALGetDriverByName("MEM") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("MEM");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_MULTIDIM_RASTER, "YES");
    poDriver->SetMetadataItem(
        GDAL_DMD_LONGNAME,
        "In Memory raster, vector and multidimensional raster");
    poDriver->SetMetadataItem(
        GDAL_DMD_CREATIONDATATYPES,
        "Byte Int8 Int16 UInt16 Int32 UInt32 Int64 UInt64 Float32 Float64 "
        "CInt16 CInt32 CFloat32 CFloat64");
    poDriver->SetMetadataItem(GDAL_DCAP_COORDINATE_EPOCH, "YES");

    poDriver->SetMetadataItem(
        GDAL_DMD_CREATIONOPTIONLIST,
        "<CreationOptionList>"
        "   <Option name='INTERLEAVE' type='string-select' default='BAND'>"
        "       <Value>BAND</Value>"
        "       <Value>PIXEL</Value>"
        "   </Option>"
        "</CreationOptionList>");

    poDriver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE_LAYER, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_DELETE_LAYER, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE_FIELD, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_DELETE_FIELD, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_REORDER_FIELDS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CURVE_GEOMETRIES, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_MEASURED_GEOMETRIES, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_Z_GEOMETRIES, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_SUPPORTED_SQL_DIALECTS, "OGRSQL SQLITE");

    poDriver->SetMetadataItem(
        GDAL_DMD_CREATIONFIELDDATATYPES,
        "Integer Integer64 Real String Date DateTime Time IntegerList "
        "Integer64List RealList StringList Binary");
    poDriver->SetMetadataItem(GDAL_DMD_CREATION_FIELD_DEFN_FLAGS,
                              "WidthPrecision Nullable Default Unique "
                              "Comment AlternativeName Domain");
    poDriver->SetMetadataItem(GDAL_DMD_ALTER_FIELD_DEFN_FLAGS,
                              "Name Type WidthPrecision Nullable Default "
                              "Unique Domain AlternativeName Comment");

    poDriver->SetMetadataItem(
        GDAL_DS_LAYER_CREATIONOPTIONLIST,
        "<LayerCreationOptionList>"
        "  <Option name='ADVERTIZE_UTF8' type='boolean' description='Whether "
        "the layer will contain UTF-8 strings' default='NO'/>"
        "  <Option name='FID' type='string' description="
        "'Name of the FID column to create' default='' />"
        "</LayerCreationOptionList>");

    poDriver->SetMetadataItem(GDAL_DCAP_COORDINATE_EPOCH, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_MULTIPLE_VECTOR_LAYERS, "YES");

    poDriver->SetMetadataItem(GDAL_DCAP_FIELD_DOMAINS, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_CREATION_FIELD_DOMAIN_TYPES,
                              "Coded Range Glob");

    poDriver->SetMetadataItem(GDAL_DMD_ALTER_GEOM_FIELD_DEFN_FLAGS,
                              "Name Type Nullable SRS CoordinateEpoch");

    // Define GDAL_NO_OPEN_FOR_MEM_DRIVER macro to undefine Open() method for
    // MEM driver.  Otherwise, bad user input can trigger easily a GDAL crash
    // as random pointers can be passed as a string.  All code in GDAL tree
    // using the MEM driver use the Create() method only, so Open() is not
    // needed, except for esoteric uses.
#ifndef GDAL_NO_OPEN_FOR_MEM_DRIVER
    poDriver->pfnOpen = MEMDataset::Open;
    poDriver->pfnIdentify = MEMDatasetIdentify;
#endif
    poDriver->pfnCreate = MEMDataset::CreateBase;
    poDriver->pfnCreateMultiDimensional = MEMDataset::CreateMultiDimensional;
    poDriver->pfnDelete = MEMDatasetDelete;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
