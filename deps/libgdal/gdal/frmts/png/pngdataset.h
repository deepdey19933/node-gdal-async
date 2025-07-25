/******************************************************************************
 *
 * Project:  PNG Driver
 * Purpose:  Implement GDAL PNG Support
 * Author:   Frank Warmerdam, warmerda@home.com
 *
 ******************************************************************************
 * Copyright (c) 2000, Frank Warmerdam
 * Copyright (c) 2007-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************
 *
 * ISSUES:
 *  o CollectMetadata() will only capture TEXT chunks before the image
 *    data as the code is currently structured.
 *  o Interlaced images are read entirely into memory for use.  This is
 *    bad for large images.
 *  o Image reading is always strictly sequential.  Reading backwards will
 *    cause the file to be rewound, and access started again from the
 *    beginning.
 *  o 16 bit alpha values are not scaled by to eight bit.
 *
 */

#include "cpl_string.h"
#include "gdal_frmts.h"
#include "gdal_pam.h"

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
// Disabled for now since currently this only works for libpng 1.2
// libpng 1.6 requires additional includes. See #6928
// #define DISABLE_CRC_CHECK
#endif

#ifdef DISABLE_CRC_CHECK
// Needs to be defined before including png.h
#define PNG_INTERNAL
#endif

#include "png.h"

#include <csetjmp>

#include <algorithm>

#ifdef _MSC_VER
#pragma warning(disable : 4611)
#endif

#ifdef USE_NEON_OPTIMIZATIONS
#define HAVE_SSE2
#include "include_sse2neon.h"
#elif defined(__SSE2__) || defined(_M_X64) ||                                  \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define HAVE_SSE2
#include <emmintrin.h>
#endif

#ifdef HAVE_SSE2
#define ENABLE_WHOLE_IMAGE_OPTIMIZATION
#endif

/************************************************************************/
/* ==================================================================== */
/*                              PNGDataset                              */
/* ==================================================================== */
/************************************************************************/

class PNGRasterBand;

#ifdef _MSC_VER
#pragma warning(push)
// 'PNGDataset': structure was padded due to __declspec(align()) at line where
// we use `jmp_buf`.
#pragma warning(disable : 4324)
#endif

class PNGDataset final : public GDALPamDataset
{
    friend class PNGRasterBand;

    VSILFILE *fpImage;
    png_structp hPNG;
    png_infop psPNGInfo;
    int nBitDepth;
    int nColorType;  // PNG_COLOR_TYPE_*
    int bInterlaced;

    int nBufferStartLine;
    int nBufferLines;
    int nLastLineRead;
    GByte *pabyBuffer;

    GDALColorTable *poColorTable;

    int bGeoTransformValid;
    double adfGeoTransform[6];

    void CollectMetadata();

    int bHasReadXMPMetadata;
    void CollectXMPMetadata();

    CPLErr LoadScanline(int);
    CPLErr LoadInterlacedChunk(int);
    void Restart();

    int bHasTriedLoadWorldFile;
    void LoadWorldFile();
    CPLString osWldFilename;

    int bHasReadICCMetadata;
    void LoadICCProfile();

    bool m_bByteOrderIsLittleEndian = false;
    bool m_bHasRewind = false;

    static void WriteMetadataAsText(jmp_buf sSetJmpContext, png_structp hPNG,
                                    png_infop psPNGInfo, const char *pszKey,
                                    const char *pszValue);
    static GDALDataset *OpenStage2(GDALOpenInfo *, PNGDataset *&);

  public:
    PNGDataset();
    virtual ~PNGDataset();

    static GDALDataset *Open(GDALOpenInfo *);
    static GDALDataset *CreateCopy(const char *pszFilename,
                                   GDALDataset *poSrcDS, int bStrict,
                                   char **papszOptions,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData);

    virtual char **GetFileList(void) override;

    virtual CPLErr GetGeoTransform(double *) override;
    virtual CPLErr FlushCache(bool bAtClosing) override;

    virtual char **GetMetadataDomainList() override;

    virtual char **GetMetadata(const char *pszDomain = "") override;
    virtual const char *
    GetMetadataItem(const char *pszName,
                    const char *pszDomain = nullptr) override;

    virtual CPLErr IRasterIO(GDALRWFlag, int, int, int, int, void *, int, int,
                             GDALDataType, int, BANDMAP_TYPE, GSpacing,
                             GSpacing, GSpacing,
                             GDALRasterIOExtraArg *psExtraArg) override;

#ifdef ENABLE_WHOLE_IMAGE_OPTIMIZATION
    bool IsCompatibleOfSingleBlock() const;
    CPLErr LoadWholeImage(void *pSingleBuffer, GSpacing nPixelSpace,
                          GSpacing nLineSpace, GSpacing nBandSpace,
                          void *apabyBuffers[4]);
#endif

    jmp_buf sSetJmpContext;  // Semi-private.

#ifdef SUPPORT_CREATE
    int m_nBitDepth;
    GByte *m_pabyBuffer;
    png_byte *m_pabyAlpha;
    png_structp m_hPNG;
    png_infop m_psPNGInfo;
    png_color *m_pasPNGColors;
    VSILFILE *m_fpImage;
    int m_bGeoTransformValid;
    double m_adfGeoTransform[6];
    char *m_pszFilename;
    int m_nColorType;  // PNG_COLOR_TYPE_*

    virtual CPLErr SetGeoTransform(double *);
    static GDALDataset *Create(const char *pszFilename, int nXSize, int nYSize,
                               int nBands, GDALDataType, char **papszParamList);

  protected:
    CPLErr write_png_header();

#endif
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

/************************************************************************/
/* ==================================================================== */
/*                            PNGRasterBand                             */
/* ==================================================================== */
/************************************************************************/

class PNGRasterBand final : public GDALPamRasterBand
{
    friend class PNGDataset;

  public:
    PNGRasterBand(PNGDataset *, int);

    virtual ~PNGRasterBand()
    {
    }

    virtual CPLErr IReadBlock(int, int, void *) override;

    virtual GDALSuggestedBlockAccessPattern
    GetSuggestedBlockAccessPattern() const override
    {
        return GSBAP_TOP_TO_BOTTOM;
    }

    virtual GDALColorInterp GetColorInterpretation() override;
    virtual GDALColorTable *GetColorTable() override;
    CPLErr SetNoDataValue(double dfNewValue) override;
    virtual double GetNoDataValue(int *pbSuccess = nullptr) override;

    virtual CPLErr IRasterIO(GDALRWFlag, int, int, int, int, void *, int, int,
                             GDALDataType, GSpacing, GSpacing,
                             GDALRasterIOExtraArg *psExtraArg) override;

    int bHaveNoData;
    double dfNoDataValue;

#ifdef SUPPORT_CREATE
    virtual CPLErr SetColorTable(GDALColorTable *);
    virtual CPLErr IWriteBlock(int, int, void *) override;

  protected:
    int m_bBandProvided[5];

    void reset_band_provision_flags()
    {
        PNGDataset &ds = *reinterpret_cast<PNGDataset *>(poDS);

        for (size_t i = 0; i < static_cast<size_t>(ds.nBands); i++)
            m_bBandProvided[i] = FALSE;
    }
#endif
};
