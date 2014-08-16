/*
  Copyright 2008 Larry Gritz and the other authors and contributors.
  All Rights Reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:
  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
  * Neither the name of the software's owners nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  (This is the Modified BSD License)
*/

/// \file
/// Implementation of ImageBufAlgo algorithms.

#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/regex.hpp>

#include <OpenEXR/half.h>
#include <OpenEXR/ImathMatrix.h>
#include <OpenEXR/ImathBox.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "OpenImageIO/imagebuf.h"
#include "OpenImageIO/imagebufalgo.h"
#include "OpenImageIO/imagebufalgo_util.h"
#include "OpenImageIO/dassert.h"
#include "OpenImageIO/platform.h"
#include "OpenImageIO/filter.h"
#include "OpenImageIO/thread.h"
#include "OpenImageIO/filesystem.h"
#include "kissfft.hh"

#ifdef USE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif


///////////////////////////////////////////////////////////////////////////
// Guidelines for ImageBufAlgo functions:
//
// * Signature will always be:
//       bool function (ImageBuf &R /* result */, 
//                      const ImageBuf &A, ...other input images...,
//                      ...other parameters...
//                      ROI roi = ROI::All(),
//                      int nthreads = 0);
// * The ROI should restrict the operation to those pixels (and channels)
//   specified. Default ROI::All() means perform the operation on all
//   pixel in R's data window.
// * It's ok to omit ROI and threads from the few functions that
//   (a) can't possibly be parallelized, and (b) do not make sense to
//   apply to anything less than the entire image.
// * Be sure to clamp the channel range to those actually used.
// * If R is initialized, do not change any pixels outside the ROI.
//   If R is uninitialized, redefine ROI to be the union of the input
//   images' data windows and allocate R to be that size.
// * Try to always do the "reasonable thing" rather than be too brittle.
// * For errors (where there is no "reasonable thing"), set R's error
//   condition using R.error() with R.error() and return false.
// * Always use IB::Iterators/ConstIterator, NEVER use getpixel/setpixel.
// * Use the iterator Black or Clamp wrap modes to avoid lots of special
//   cases inside the pixel loops.
// * Use OIIO_DISPATCH_* macros to call type-specialized templated
//   implemenations.  It is permissible to use OIIO_DISPATCH_COMMON_TYPES_*
//   to tame the cross-product of types, especially for binary functions
//   (A,B inputs as well as R output).
///////////////////////////////////////////////////////////////////////////


OIIO_NAMESPACE_ENTER
{


// Convenient helper struct to bundle a 3-int describing a block size.
struct Dim3 {
    int x, y, z;
    Dim3 (int x, int y=1, int z=1) : x(x), y(y), z(z) { }
};



bool
ImageBufAlgo::IBAprep (ROI &roi, ImageBuf *dst,
                       const ImageBuf *A, const ImageBuf *B,
                       ImageSpec *force_spec, int prepflags)
{
    if ((A && !A->initialized()) || (B && !B->initialized())) {
        if (dst)
            dst->error ("Uninitialized input image");
        return false;
    }
    if (dst->initialized()) {
        // Valid destination image.  Just need to worry about ROI.
        if (roi.defined()) {
            // Shrink-wrap ROI to the destination (including chend)
            roi = roi_intersection (roi, get_roi(dst->spec()));
        } else {
            // No ROI? Set it to all of dst's pixel window.
            roi = get_roi (dst->spec());
        }
        // If the dst is initialized but is a cached image, we'll need
        // to fully read it into allocated memory so that we're able
        // to write to it subsequently.
        if (dst->storage() == ImageBuf::IMAGECACHE) {
            dst->read (dst->subimage(), dst->miplevel(), true /*force*/);
            ASSERT (dst->storage() == ImageBuf::LOCALBUFFER);
        }
    } else {
        // Not an initialized destination image!
        ASSERT ((A || roi.defined()) &&
                "ImageBufAlgo without any guess about region of interest");
        ROI full_roi;
        if (! roi.defined()) {
            // No ROI -- make it the union of the pixel regions of the inputs
            roi = get_roi (A->spec());
            full_roi = get_roi_full (A->spec());
            if (B) {
                roi = roi_union (roi, get_roi (B->spec()));
                full_roi = roi_union (full_roi, get_roi_full (B->spec()));
            }
        } else {
            if (A) {
                roi.chend = std::min (roi.chend, A->nchannels());
                if (! (prepflags & IBAprep_NO_COPY_ROI_FULL))
                    full_roi = A->roi_full();
            } else {
                full_roi = roi;
            }
        }
        // Now we allocate space for dst.  Give it A's spec, but adjust
        // the dimensions to match the ROI.
        ImageSpec spec;
        if (A) {
            // If there's an input image, give dst A's spec (with
            // modifications detailed below...)
            spec = force_spec ? (*force_spec) : A->spec();
            // For two inputs, if they aren't the same data type, punt and
            // allocate a float buffer. If the user wanted something else,
            // they should have pre-allocated dst with their desired format.
            if (B && A->spec().format != B->spec().format)
                spec.set_format (TypeDesc::FLOAT);
            // No good can come from automatically polluting an ImageBuf
            // with some other ImageBuf's tile sizes.
            spec.tile_width = 0;
            spec.tile_height = 0;
            spec.tile_depth = 0;
        } else if (force_spec) {
            spec = *force_spec;
        } else {
            spec.set_format (TypeDesc::FLOAT);
            spec.nchannels = roi.chend;
            spec.default_channel_names ();
        }
        // Set the image dimensions based on ROI.
        set_roi (spec, roi);
        if (full_roi.defined())
            set_roi_full (spec, full_roi);
        else
            set_roi_full (spec, roi);

        if (prepflags & IBAprep_NO_COPY_METADATA)
            spec.extra_attribs.clear();
        else if (! (prepflags & IBAprep_COPY_ALL_METADATA)) {
            // Since we're altering pixels, be sure that any existing SHA
            // hash of dst's pixel values is erased.
            spec.erase_attribute ("oiio:SHA-1");
            static boost::regex regex_sha ("SHA-1=[[:xdigit:]]*[ ]*");
            std::string desc = spec.get_string_attribute ("ImageDescription");
            if (desc.size())
                spec.attribute ("ImageDescription",
                                boost::regex_replace (desc, regex_sha, ""));
        }

        dst->alloc (spec);
    }
    if (prepflags & IBAprep_REQUIRE_ALPHA) {
        if (dst->spec().alpha_channel < 0 ||
              (A && A->spec().alpha_channel < 0) ||
              (B && B->spec().alpha_channel < 0)) {
            dst->error ("images must have alpha channels");
            return false;
        }
    }
    if (prepflags & IBAprep_REQUIRE_Z) {
        if (dst->spec().z_channel < 0 ||
              (A && A->spec().z_channel < 0) ||
              (B && B->spec().z_channel < 0)) {
            dst->error ("images must have depth channels");
            return false;
        }
    }
    if (prepflags & IBAprep_REQUIRE_SAME_NCHANNELS) {
        int n = dst->spec().nchannels;
        if ((A && A->spec().nchannels != n) ||
            (B && B->spec().nchannels != n)) {
            dst->error ("images must have the same number of channels");
            return false;
        }
    }
    if (prepflags & IBAprep_NO_SUPPORT_VOLUME) {
        if (dst->spec().depth > 1 ||
                (A && A->spec().depth > 1) ||
                (B && B->spec().depth > 1)) {
            dst->error ("volumes not supported");
            return false;
        }
    }
    return true;
}



template<typename T>
static bool
fill_ (ImageBuf &dst, const float *values, ROI roi=ROI(), int nthreads=1)
{
    if (nthreads != 1 && roi.npixels() >= 1000) {
        // Lots of pixels and request for multi threads? Parallelize.
        ImageBufAlgo::parallel_image (
            boost::bind(fill_<T>, boost::ref(dst), values,
                        _1 /*roi*/, 1 /*nthreads*/),
            roi, nthreads);
        return true;
    }

    // Serial case
    for (ImageBuf::Iterator<T> p (dst, roi);  !p.done();  ++p)
        for (int c = roi.chbegin;  c < roi.chend;  ++c)
            p[c] = values[c];
    return true;
}


bool
ImageBufAlgo::fill (ImageBuf &dst, const float *pixel, ROI roi, int nthreads)
{
    ASSERT (pixel && "fill must have a non-NULL pixel value pointer");
    if (! IBAprep (roi, &dst))
        return false;
    bool ok;
    OIIO_DISPATCH_TYPES (ok, "fill", fill_, dst.spec().format,
                         dst, pixel, roi, nthreads);
    return ok;
}


bool
ImageBufAlgo::zero (ImageBuf &dst, ROI roi, int nthreads)
{
    if (! IBAprep (roi, &dst))
        return false;
    float *zero = ALLOCA(float,roi.chend);
    memset (zero, 0, roi.chend*sizeof(float));
    return fill (dst, zero, roi, nthreads);
}



template<typename T>
static bool
checker_ (ImageBuf &dst, Dim3 size,
          const float *color1, const float *color2,
          Dim3 offset,
          ROI roi, int nthreads=1)
{
    if (nthreads != 1 && roi.npixels() >= 1000) {
        // Lots of pixels and request for multi threads? Parallelize.
        ImageBufAlgo::parallel_image (
            boost::bind(checker_<T>, boost::ref(dst),
                        size, color1, color2, offset,
                        _1 /*roi*/, 1 /*nthreads*/),
            roi, nthreads);
        return true;
    }

    // Serial case
    for (ImageBuf::Iterator<T> p (dst, roi);  !p.done();  ++p) {
        int xtile = (p.x()-offset.x)/size.x;  xtile += (p.x()<offset.x);
        int ytile = (p.y()-offset.y)/size.y;  ytile += (p.y()<offset.y);
        int ztile = (p.z()-offset.z)/size.z;  ztile += (p.z()<offset.z);
        int v = xtile + ytile + ztile;
        if (v & 1)
            for (int c = roi.chbegin;  c < roi.chend;  ++c)
                p[c] = color2[c];
        else
            for (int c = roi.chbegin;  c < roi.chend;  ++c)
                p[c] = color1[c];
    }
    return true;
}



bool
ImageBufAlgo::checker (ImageBuf &dst, int width, int height, int depth,
                       const float *color1, const float *color2,
                       int xoffset, int yoffset, int zoffset,
                       ROI roi, int nthreads)
{
    if (! IBAprep (roi, &dst))
        return false;
    bool ok;
    OIIO_DISPATCH_TYPES (ok, "checker", checker_, dst.spec().format,
                         dst, Dim3(width, height, depth), color1, color2,
                         Dim3(xoffset, yoffset, zoffset), roi, nthreads);
    return ok;
}



template<typename DSTTYPE, typename SRCTYPE>
static bool
resize_ (ImageBuf &dst, const ImageBuf &src,
         Filter2D *filter, ROI roi, int nthreads)
{
    if (nthreads != 1 && roi.npixels() >= 1000) {
        // Lots of pixels and request for multi threads? Parallelize.
        ImageBufAlgo::parallel_image (
            boost::bind(resize_<DSTTYPE,SRCTYPE>, boost::ref(dst),
                        boost::cref(src), filter,
                        _1 /*roi*/, 1 /*nthreads*/),
            roi, nthreads);
        return true;
    }

    // Serial case

    const ImageSpec &srcspec (src.spec());
    const ImageSpec &dstspec (dst.spec());
    int nchannels = dstspec.nchannels;

    // Local copies of the source image window, converted to float
    float srcfx = srcspec.full_x;
    float srcfy = srcspec.full_y;
    float srcfw = srcspec.full_width;
    float srcfh = srcspec.full_height;

    // Ratios of dst/src size.  Values larger than 1 indicate that we
    // are maximizing (enlarging the image), and thus want to smoothly
    // interpolate.  Values less than 1 indicate that we are minimizing
    // (shrinking the image), and thus want to properly filter out the
    // high frequencies.
    float xratio = float(dstspec.full_width) / srcfw; // 2 upsize, 0.5 downsize
    float yratio = float(dstspec.full_height) / srcfh;

    float dstfx = dstspec.full_x;
    float dstfy = dstspec.full_y;
    float dstfw = dstspec.full_width;
    float dstfh = dstspec.full_height;
    float dstpixelwidth = 1.0f / dstfw;
    float dstpixelheight = 1.0f / dstfh;
    float *pel = ALLOCA (float, nchannels);
    float filterrad = filter->width() / 2.0f;

    // radi,radj is the filter radius, as an integer, in source pixels.  We
    // will filter the source over [x-radi, x+radi] X [y-radj,y+radj].
    int radi = (int) ceilf (filterrad/xratio);
    int radj = (int) ceilf (filterrad/yratio);
    int xtaps = 2*radi + 1;
    int ytaps = 2*radj + 1;
    bool separable = filter->separable();
    float *xfiltval = NULL, *yfiltval = NULL;
    if (separable) {
        // Allocate temp space to cache the filter weights
        xfiltval = ALLOCA (float, xtaps);
        yfiltval = ALLOCA (float, ytaps);
    }
#if 0
    std::cerr << "Resizing " << srcspec.full_width << "x" << srcspec.full_height
              << " to " << dstspec.full_width << "x" << dstspec.full_height << "\n";
    std::cerr << "ratios = " << xratio << ", " << yratio << "\n";
    std::cerr << "examining src filter support radius of " << radi << " x " << radj << " pixels\n";
    std::cerr << "dst range " << roi << "\n";
    std::cerr << "separable filter\n";
#endif


    // We're going to loop over all output pixels we're interested in.
    //
    // (s,t) = NDC space coordinates of the output sample we are computing.
    //     This is the "sample point".
    // (src_xf, src_xf) = source pixel space float coordinates of the
    //     sample we're computing. We want to compute the weighted sum
    //     of all the source image pixels that fall under the filter when
    //     centered at that location.
    // (src_x, src_y) = image space integer coordinates of the floor,
    //     i.e., the closest pixel in the source image.
    // src_xf_frac and src_yf_frac are the position within that pixel
    //     of our sample.
    ImageBuf::Iterator<DSTTYPE> out (dst, roi);
    for (int y = roi.ybegin;  y < roi.yend;  ++y) {
        float t = (y-dstfy+0.5f)*dstpixelheight;
        float src_yf = srcfy + t * srcfh;
        int src_y;
        float src_yf_frac = floorfrac (src_yf, &src_y);

        // If using separable filters, our vertical set of filter tap
        // weights will be the same for the whole scanline we're on.  Just
        // compute and normalize them once.
        float totalweight_y = 0.0f;
        if (separable) {
            for (int j = 0;  j < ytaps;  ++j) {
                float w = filter->yfilt (yratio * (j-radj-(src_yf_frac-0.5f)));
                yfiltval[j] = w;
                totalweight_y += w;
            }
            for (int i = 0;  i < ytaps;  ++i)
                yfiltval[i] /= totalweight_y;
        }

        for (int x = roi.xbegin;  x < roi.xend;  ++x) {
            float s = (x-dstfx+0.5f)*dstpixelwidth;
            float src_xf = srcfx + s * srcfw;
            int src_x;
            float src_xf_frac = floorfrac (src_xf, &src_x);
            for (int c = 0;  c < nchannels;  ++c)
                pel[c] = 0.0f;
            if (separable) {
                // Cache and normalize the horizontal filter tap weights
                // just once for this (x,y) position, reuse for all vertical
                // taps.
                float totalweight_x = 0.0f;
                for (int i = 0;  i < xtaps;  ++i) {
                    float w = filter->xfilt (xratio * (i-radi-(src_xf_frac-0.5f)));
                    xfiltval[i] = w;
                    totalweight_x += w;
                }

                if (totalweight_x != 0.0f) {
                    for (int i = 0;  i < xtaps;  ++i)  // normalize x filter
                        xfiltval[i] /= totalweight_x;  // weights
                    ImageBuf::ConstIterator<SRCTYPE> srcpel (src, src_x-radi, src_x+radi+1,
                                                             src_y-radj, src_y+radj+1,
                                                             0, 1, ImageBuf::WrapClamp);
                    for (int j = -radj;  j <= radj;  ++j) {
                        float wy = yfiltval[j+radj];
                        if (wy == 0.0f) {
                            // 0 weight for this y tap -- move to next line
                            srcpel.pos (srcpel.x(), srcpel.y()+1, srcpel.z());
                            continue;
                        }
                        for (int i = 0;  i < xtaps; ++i, ++srcpel) {
                            float w = wy * xfiltval[i];
                            for (int c = 0;  c < nchannels;  ++c)
                                pel[c] += w * srcpel[c];
                        }
                    }
                }
                // Copy the pixel value (already normalized) to the output.
                DASSERT (out.x() == x && out.y() == y);
                if (totalweight_y == 0.0f) {
                    // zero it out
                    for (int c = 0;  c < nchannels;  ++c)
                        out[c] = 0.0f;
                } else {
                    for (int c = 0;  c < nchannels;  ++c)
                        out[c] = pel[c];
                }
            } else {
                // Non-separable
                float totalweight = 0.0f;
                ImageBuf::ConstIterator<SRCTYPE> srcpel (src, src_x-radi, src_x+radi+1,
                                                       src_y-radi, src_y+radi+1,
                                                       0, 1, ImageBuf::WrapClamp);
                for (int j = -radj;  j <= radj;  ++j) {
                    for (int i = -radi;  i <= radi;  ++i, ++srcpel) {
                        float w = (*filter)(xratio * (i-(src_xf_frac-0.5f)),
                                            yratio * (j-(src_yf_frac-0.5f)));
                        totalweight += w;
                        if (w == 0.0f)
                            continue;
                        DASSERT (! srcpel.done());
                        for (int c = 0;  c < nchannels;  ++c)
                            pel[c] += w * srcpel[c];
                    }
                }
                DASSERT (srcpel.done());
                // Rescale pel to normalize the filter and write it to the
                // output image.
                DASSERT (out.x() == x && out.y() == y);
                if (totalweight == 0.0f) {
                    // zero it out
                    for (int c = 0;  c < nchannels;  ++c)
                        out[c] = 0.0f;
                } else {
                    for (int c = 0;  c < nchannels;  ++c)
                        out[c] = pel[c] / totalweight;
                }
            }

            ++out;
        }
    }

    return true;
}



bool
ImageBufAlgo::resize (ImageBuf &dst, const ImageBuf &src,
                      Filter2D *filter, ROI roi, int nthreads)
{
    if (! IBAprep (roi, &dst, &src,
            IBAprep_REQUIRE_SAME_NCHANNELS | IBAprep_NO_SUPPORT_VOLUME |
            IBAprep_NO_COPY_ROI_FULL))
        return false;

    // Set up a shared pointer with custom deleter to make sure any
    // filter we allocate here is properly destroyed.
    boost::shared_ptr<Filter2D> filterptr ((Filter2D*)NULL, Filter2D::destroy);
    bool allocfilter = (filter == NULL);
    if (allocfilter) {
        // If no filter was provided, punt and just linearly interpolate.
        const ImageSpec &srcspec (src.spec());
        const ImageSpec &dstspec (dst.spec());
        float wratio = float(dstspec.full_width) / float(srcspec.full_width);
        float hratio = float(dstspec.full_height) / float(srcspec.full_height);
        float w = 2.0f * std::max (1.0f, wratio);
        float h = 2.0f * std::max (1.0f, hratio);
        filter = Filter2D::create ("triangle", w, h);
        filterptr.reset (filter);
    }

    bool ok;
    OIIO_DISPATCH_TYPES2 (ok, "resize", resize_,
                          dst.spec().format, src.spec().format,
                          dst, src, filter, roi, nthreads);
    return ok;
}



bool
ImageBufAlgo::resize (ImageBuf &dst, const ImageBuf &src,
                      const std::string &filtername_, float fwidth,
                      ROI roi, int nthreads)
{
    if (! IBAprep (roi, &dst, &src,
            IBAprep_REQUIRE_SAME_NCHANNELS | IBAprep_NO_SUPPORT_VOLUME |
            IBAprep_NO_COPY_ROI_FULL))
        return false;
    const ImageSpec &srcspec (src.spec());
    const ImageSpec &dstspec (dst.spec());

    // Resize ratios
    float wratio = float(dstspec.full_width) / float(srcspec.full_width);
    float hratio = float(dstspec.full_height) / float(srcspec.full_height);

    // Set up a shared pointer with custom deleter to make sure any
    // filter we allocate here is properly destroyed.
    boost::shared_ptr<Filter2D> filter ((Filter2D*)NULL, Filter2D::destroy);
    std::string filtername = filtername_;
    if (filtername.empty()) {
        // No filter name supplied -- pick a good default
        if (wratio > 1.0f || hratio > 1.0f)
            filtername = "blackman-harris";
        else
            filtername = "lanczos3";
    }
    for (int i = 0, e = Filter2D::num_filters();  i < e;  ++i) {
        FilterDesc fd;
        Filter2D::get_filterdesc (i, &fd);
        if (fd.name == filtername) {
            float w = fwidth > 0.0f ? fwidth : fd.width * std::max (1.0f, wratio);
            float h = fwidth > 0.0f ? fwidth : fd.width * std::max (1.0f, hratio);
            filter.reset (Filter2D::create (filtername, w, h));
            break;
        }
    }
    if (! filter) {
        dst.error ("Filter \"%s\" not recognized", filtername);
        return false;
    }

    bool ok;
    OIIO_DISPATCH_TYPES2 (ok, "resize", resize_,
                          dstspec.format, srcspec.format,
                          dst, src, filter.get(), roi, nthreads);
    return ok;
}



template<typename DSTTYPE, typename SRCTYPE>
static bool
resample_ (ImageBuf &dst, const ImageBuf &src, bool interpolate,
           ROI roi, int nthreads)
{
    if (nthreads != 1 && roi.npixels() >= 1000) {
        // Lots of pixels and request for multi threads? Parallelize.
        ImageBufAlgo::parallel_image (
            boost::bind(resample_<DSTTYPE,SRCTYPE>, boost::ref(dst),
                        boost::cref(src), interpolate,
                        _1 /*roi*/, 1 /*nthreads*/),
            roi, nthreads);
        return true;
    }

    // Serial case

    const ImageSpec &srcspec (src.spec());
    const ImageSpec &dstspec (dst.spec());
    int nchannels = src.nchannels();

    // Local copies of the source image window, converted to float
    float srcfx = srcspec.full_x;
    float srcfy = srcspec.full_y;
    float srcfw = srcspec.full_width;
    float srcfh = srcspec.full_height;

    float dstfx = dstspec.full_x;
    float dstfy = dstspec.full_y;
    float dstfw = dstspec.full_width;
    float dstfh = dstspec.full_height;
    float dstpixelwidth = 1.0f / dstfw;
    float dstpixelheight = 1.0f / dstfh;
    float *pel = ALLOCA (float, nchannels);

    ImageBuf::Iterator<DSTTYPE> out (dst, roi);
    ImageBuf::ConstIterator<SRCTYPE> srcpel (src);
    for (int y = roi.ybegin;  y < roi.yend;  ++y) {
        // s,t are NDC space
        float t = (y-dstfy+0.5f)*dstpixelheight;
        // src_xf, src_xf are image space float coordinates
        float src_yf = srcfy + t * srcfh - 0.5f;
        // src_x, src_y are image space integer coordinates of the floor
        int src_y;
        (void) floorfrac (src_yf, &src_y);
        for (int x = roi.xbegin;  x < roi.xend;  ++x) {
            float s = (x-dstfx+0.5f)*dstpixelwidth;
            float src_xf = srcfx + s * srcfw - 0.5f;
            int src_x;
            (void) floorfrac (src_xf, &src_x);

            if (interpolate) {
                src.interppixel (src_xf, src_yf, pel);
                for (int c = roi.chbegin; c < roi.chend; ++c)
                    out[c] = pel[c];
            } else {
                srcpel.pos (src_x, src_y, 0);
                for (int c = roi.chbegin; c < roi.chend; ++c)
                    out[c] = srcpel[c];
            }
            ++out;
        }
    }

    return true;
}



bool
ImageBufAlgo::resample (ImageBuf &dst, const ImageBuf &src,
                        bool interpolate, ROI roi, int nthreads)
{
    if (! IBAprep (roi, &dst, &src,
            IBAprep_REQUIRE_SAME_NCHANNELS | IBAprep_NO_SUPPORT_VOLUME |
            IBAprep_NO_COPY_ROI_FULL))
        return false;
    bool ok;
    OIIO_DISPATCH_TYPES2 (ok, "resample", resample_,
                          dst.spec().format, src.spec().format,
                          dst, src, interpolate, roi, nthreads);
    return ok;
}



#if 0
template<typename DSTTYPE, typename SRCTYPE>
static bool
affine_resample_ (ImageBuf &dst, const ImageBuf &src, const Imath::M33f &Minv,
                  ROI roi, int nthreads)
{
    if (nthreads != 1 && roi.npixels() >= 1000) {
        // Possible multiple thread case -- recurse via parallel_image
        ImageBufAlgo::parallel_image (
            boost::bind(affine_resample_<DSTTYPE,SRCTYPE>,
                        boost::ref(dst), boost::cref(src), Minv,
                        _1 /*roi*/, 1 /*nthreads*/),
            roi, nthreads);
        return true;
    }

    // Serial case
    ImageBuf::Iterator<DSTTYPE,DSTTYPE> d (dst, roi);
    ImageBuf::ConstIterator<SRCTYPE,DSTTYPE> s (src);
    for (  ;  ! d.done();  ++d) {
        Imath::V2f P (d.x()+0.5f, d.y()+0.5f);
        Minv.multVecMatrix (P, P);
        s.pos (int(floorf(P.x)), int(floorf(P.y)), d.z());
        for (int c = roi.chbegin;  c < roi.chend;  ++c)
            d[c] = s[c];
    }
    return true;
}
#endif



namespace {

// Poor man's Dual2<float> makes it easy to compute with differentials. For
// a rich man's implementation and full documentation, see
// OpenShadingLanguage (dual2.h).
class Dual2 {
public:
    float val() const { return m_val; }
    float dx() const { return m_dx; }
    float dy() const { return m_dy; }
    Dual2 (float val) : m_val(val), m_dx(0.0f), m_dy(0.0f) {}
    Dual2 (float val, float dx, float dy) : m_val(val), m_dx(dx), m_dy(dy) {}
    Dual2& operator= (float f) { m_val = f; m_dx = 0.0f; m_dy = 0.0f; return *this; }
    friend Dual2 operator+ (const Dual2 &a, const Dual2 &b) {
        return Dual2 (a.m_val+b.m_val, a.m_dx+b.m_dx, a.m_dy+b.m_dy);
    }
    friend Dual2 operator+ (const Dual2 &a, float b) {
        return Dual2 (a.m_val+b, a.m_dx, a.m_dy);
    }
    friend Dual2 operator* (const Dual2 &a, float b) {
        return Dual2 (a.m_val*b, a.m_dx*b, a.m_dy*b);
    }
    friend Dual2 operator* (const Dual2 &a, const Dual2 &b) {
        // Use the chain rule
        return Dual2 (a.m_val*b.m_val,
                      a.m_val*b.m_dx + a.m_dx*b.m_val,
                      a.m_val*b.m_dy + a.m_dy*b.m_val);
    }
    friend Dual2 operator/ (const Dual2 &a, const Dual2 &b) {
        float bvalinv = 1.0f / b.m_val;
        float aval_bval = a.m_val * bvalinv;
        return Dual2 (aval_bval,
                      bvalinv * (a.m_dx - aval_bval * b.m_dx),
                      bvalinv * (a.m_dy - aval_bval * b.m_dy));
    }
private:
    float m_val, m_dx, m_dy;
};

} // end anon namespace


/// Transform a 2D point (x,y) with derivatives by a 3x3 affine matrix to
/// obtain a transformed point with derivatives.
inline void
robust_multVecMatrix (const Imath::M33f &M,
                      const Dual2 &x, const Dual2 &y,
                      Dual2 &outx, Dual2 &outy)
{
    Dual2 a = x * M[0][0] + y * M[1][0] + M[2][0];
    Dual2 b = x * M[0][1] + y * M[1][1] + M[2][1];
    Dual2 w = x * M[0][2] + y * M[1][2] + M[2][2];

    if (w.val() != 0.0f) {
       outx = a / w;
       outy = b / w;
    } else {
       outx = 0.0f;
       outy = 0.0f;
    }
}



// Given s,t image space coordinates and their derivatives, compute a 
// filtered sample using the derivatives to guide the size of the filter
// footprint.
template<typename SRCTYPE>
inline void
filtered_sample (const ImageBuf &src, float s, float t,
                 float dsdx, float dtdx, float dsdy, float dtdy,
                 const Filter2D *filter, ImageBuf::WrapMode wrap,
                 float *result)
{
    DASSERT (filter);
    // Just use isotropic filtering
    float ds = std::max (1.0f, std::max (fabsf(dsdx), fabsf(dsdy)));
    float dt = std::max (1.0f, std::max (fabsf(dtdx), fabsf(dtdy)));
    float ds_inv = 1.0f / ds;
    float dt_inv = 1.0f / dt;
    float filterrad_s = 0.5f * ds * filter->width();
    float filterrad_t = 0.5f * dt * filter->width();
    ImageBuf::ConstIterator<SRCTYPE> samp (src, 
                      (int)floorf(s-filterrad_s), (int)ceilf(s+filterrad_s),
                      (int)floorf(t-filterrad_t), (int)ceilf(t+filterrad_t),
                      0, 1, wrap);
    int nc = src.nchannels();
    float *sum = ALLOCA (float, nc);
    memset (sum, 0, nc*sizeof(float));
    float total_w = 0.0f;
    for ( ; ! samp.done(); ++samp) {
        float w = (*filter) (ds_inv*(samp.x()+0.5f-s), dt_inv*(samp.y()+0.5f-t));
        for (int c = 0; c < nc; ++c)
            sum[c] += w * samp[c];
        total_w += w;
    }
    if (total_w != 0.0f)
        for (int c = 0; c < nc; ++c)
            result[c] = sum[c] / total_w;
    else
        for (int c = 0; c < nc; ++c)
            result[c] = 0.0f;
}



template<typename DSTTYPE, typename SRCTYPE>
static bool
warp_ (ImageBuf &dst, const ImageBuf &src, const Imath::M33f &M,
       const Filter2D *filter, ImageBuf::WrapMode wrap,
       ROI roi, int nthreads)
{
    if (nthreads != 1 && roi.npixels() >= 1000) {
        // Possible multiple thread case -- recurse via parallel_image
        ImageBufAlgo::parallel_image (
            boost::bind(warp_<DSTTYPE,SRCTYPE>,
                        boost::ref(dst), boost::cref(src), M,
                        filter, wrap, _1 /*roi*/, 1 /*nthreads*/),
            roi, nthreads);
        return true;
    }

    // Serial case
    int nc = dst.nchannels();
    float *pel = ALLOCA (float, nc);
    memset (pel, 0, nc*sizeof(float));
    Imath::M33f Minv = M.inverse();
    ImageBuf::Iterator<DSTTYPE> out (dst, roi);
    for (  ;  ! out.done();  ++out) {
        Dual2 x (out.x()+0.5f, 1.0f, 0.0f);
        Dual2 y (out.y()+0.5f, 0.0f, 1.0f);
        robust_multVecMatrix (Minv, x, y, x, y);
        filtered_sample<SRCTYPE> (src, x.val(), y.val(),
                                  x.dx(), y.dx(), x.dy(), y.dy(),
                                  filter, wrap, pel);
        for (int c = roi.chbegin;  c < roi.chend;  ++c)
            out[c] = pel[c];
 
    }
    return true;
}



// Transform an ROI by an affine matrix.
static ROI
transform (const Imath::M33f &M, ROI roi)
{
    Imath::V2f ul (roi.xbegin+0.5f, roi.ybegin+0.5f);
    Imath::V2f ur (roi.xend-0.5f, roi.ybegin+0.5f);
    Imath::V2f ll (roi.xbegin+0.5f, roi.yend-0.5f);
    Imath::V2f lr (roi.xend-0.5f, roi.yend-0.5f);
    M.multVecMatrix (ul, ul);
    M.multVecMatrix (ur, ur);
    M.multVecMatrix (ll, ll);
    M.multVecMatrix (lr, lr);
    Imath::Box2f box (ul);
    box.extendBy (ll);
    box.extendBy (ur);
    box.extendBy (lr);
    int xmin = int (floorf(box.min.x));
    int ymin = int (floorf(box.min.y));
    int xmax = int (floorf(box.max.x)) + 1;
    int ymax = int (floorf(box.max.y)) + 1;
    return ROI (xmin, xmax, ymin, ymax, roi.zbegin, roi.zend, roi.chbegin, roi.chend);
}



bool
ImageBufAlgo::warp (ImageBuf &dst, const ImageBuf &src,
                    const Imath::M33f &M,
                    const Filter2D *filter,
                    bool recompute_roi, ImageBuf::WrapMode wrap,
                    ROI roi, int nthreads)
{
    ROI src_roi_full = src.roi_full();
    ROI dst_roi, dst_roi_full;
    if (dst.initialized()) {
        dst_roi = roi.defined() ? roi : dst.roi();
        dst_roi_full = dst.roi_full();
    } else {
        dst_roi = roi.defined() ? roi : (recompute_roi ? transform (M, src.roi()) : src.roi());
        dst_roi_full = src_roi_full;
    }
    dst_roi.chend = std::min (dst_roi.chend, src.nchannels());
    dst_roi_full.chend = std::min (dst_roi_full.chend, src.nchannels());

    if (! IBAprep (dst_roi, &dst, &src, IBAprep_NO_SUPPORT_VOLUME))
        return false;

    // Set up a shared pointer with custom deleter to make sure any
    // filter we allocate here is properly destroyed.
    boost::shared_ptr<Filter2D> filterptr ((Filter2D*)NULL, Filter2D::destroy);
    if (filter == NULL) {
        // If no filter was provided, punt and just linearly interpolate.
        filterptr.reset (Filter2D::create ("lanczos3", 6.0f, 6.0f));
        filter = filterptr.get();
    }

    bool ok;
    OIIO_DISPATCH_TYPES2 (ok, "warp", warp_,
                          dst.spec().format, src.spec().format,
                          dst, src, M, filter, wrap, dst_roi, nthreads);
    return ok;
}



bool
ImageBufAlgo::warp (ImageBuf &dst, const ImageBuf &src,
                    const Imath::M33f &M,
                    string_view filtername_, float filterwidth,
                    bool recompute_roi, ImageBuf::WrapMode wrap,
                    ROI roi, int nthreads)
{
    // Set up a shared pointer with custom deleter to make sure any
    // filter we allocate here is properly destroyed.
    boost::shared_ptr<Filter2D> filter ((Filter2D*)NULL, Filter2D::destroy);
    std::string filtername = filtername_.size() ? filtername_ : "lanczos3";
    for (int i = 0, e = Filter2D::num_filters();  i < e;  ++i) {
        FilterDesc fd;
        Filter2D::get_filterdesc (i, &fd);
        if (fd.name == filtername) {
            float w = filterwidth > 0.0f ? filterwidth : fd.width;
            float h = filterwidth > 0.0f ? filterwidth : fd.width;
            filter.reset (Filter2D::create (filtername, w, h));
            break;
        }
    }
    if (! filter) {
        dst.error ("Filter \"%s\" not recognized", filtername);
        return false;
    }

    return warp (dst, src, M, filter.get(), recompute_roi, wrap, roi, nthreads);
}



bool
ImageBufAlgo::rotate (ImageBuf &dst, const ImageBuf &src,
                      float angle, float center_x, float center_y,
                      Filter2D *filter, bool recompute_roi,
                      ROI roi, int nthreads)
{
    // Calculate the rotation matrix
    Imath::M33f M;
    M.translate(Imath::V2f(-center_x, -center_y));
    M.rotate(angle);
    M *= Imath::M33f().translate(Imath::V2f(center_x, center_y));
    return ImageBufAlgo::warp (dst, src, M, filter,
                               recompute_roi, ImageBuf::WrapBlack,
                               roi, nthreads);
}



bool
ImageBufAlgo::rotate (ImageBuf &dst, const ImageBuf &src,
                      float angle, float center_x, float center_y,
                      string_view filtername, float filterwidth,
                      bool recompute_roi, ROI roi, int nthreads)
{
    // Calculate the rotation matrix
    Imath::M33f M;
    M.translate(Imath::V2f(-center_x, -center_y));
    M.rotate(angle);
    M *= Imath::M33f().translate(Imath::V2f(center_x, center_y));
    return ImageBufAlgo::warp (dst, src, M, filtername, filterwidth,
                               recompute_roi, ImageBuf::WrapBlack,
                               roi, nthreads);
}



bool
ImageBufAlgo::rotate (ImageBuf &dst, const ImageBuf &src, float angle,
                      Filter2D *filter,
                      bool recompute_roi, ROI roi, int nthreads)
{
    ROI src_roi_full = src.roi_full();
    float center_x = 0.5f * (src_roi_full.xbegin + src_roi_full.xend);
    float center_y = 0.5f * (src_roi_full.ybegin + src_roi_full.yend);
    return ImageBufAlgo::rotate (dst, src, angle, center_x, center_y,
                                 filter, recompute_roi, roi, nthreads);
}



bool
ImageBufAlgo::rotate (ImageBuf &dst, const ImageBuf &src, float angle,
                      string_view filtername, float filterwidth,
                      bool recompute_roi,
                      ROI roi, int nthreads)
{
    ROI src_roi_full = src.roi_full();
    float center_x = 0.5f * (src_roi_full.xbegin + src_roi_full.xend);
    float center_y = 0.5f * (src_roi_full.ybegin + src_roi_full.yend);
    return ImageBufAlgo::rotate (dst, src, angle, center_x, center_y,
                                 filtername, filterwidth, recompute_roi,
                                 roi, nthreads);
}



template<typename DSTTYPE, typename SRCTYPE>
static bool
convolve_ (ImageBuf &dst, const ImageBuf &src, const ImageBuf &kernel,
           bool normalize, ROI roi, int nthreads)
{
    if (nthreads != 1 && roi.npixels() >= 1000) {
        // Lots of pixels and request for multi threads? Parallelize.
        ImageBufAlgo::parallel_image (
            boost::bind(convolve_<DSTTYPE,SRCTYPE>, boost::ref(dst),
                        boost::cref(src), boost::cref(kernel), normalize,
                        _1 /*roi*/, 1 /*nthreads*/),
            roi, nthreads);
        return true;
    }

    // Serial case

    float scale = 1.0f;
    if (normalize) {
        scale = 0.0f;
        for (ImageBuf::ConstIterator<float> k (kernel); ! k.done(); ++k)
            scale += k[0];
        scale = 1.0f / scale;
    }

    float *sum = ALLOCA (float, roi.chend);
    ROI kroi = get_roi (kernel.spec());
    ImageBuf::Iterator<DSTTYPE> d (dst, roi);
    ImageBuf::ConstIterator<SRCTYPE> s (src, roi, ImageBuf::WrapClamp);
    for ( ; ! d.done();  ++d) {

        for (int c = roi.chbegin; c < roi.chend; ++c)
            sum[c] = 0.0f;

        for (ImageBuf::ConstIterator<float> k (kernel, kroi); !k.done(); ++k) {
            float kval = k[0];
            s.pos (d.x() + k.x(), d.y() + k.y(), d.z() + k.z());
            for (int c = roi.chbegin; c < roi.chend; ++c)
                sum[c] += kval * s[c];
        }
        
        for (int c = roi.chbegin; c < roi.chend; ++c)
            d[c] = scale * sum[c];
    }

    return true;
}



bool
ImageBufAlgo::convolve (ImageBuf &dst, const ImageBuf &src,
                        const ImageBuf &kernel, bool normalize,
                        ROI roi, int nthreads)
{
    if (! IBAprep (roi, &dst, &src))
        return false;
    if (dst.nchannels() != src.nchannels()) {
        dst.error ("channel number mismatch: %d vs. %d", 
                   dst.spec().nchannels, src.spec().nchannels);
        return false;
    }
    bool ok;
    OIIO_DISPATCH_TYPES2 (ok, "convolve", convolve_,
                          dst.spec().format, src.spec().format,
                          dst, src, kernel, normalize, roi, nthreads);
    return ok;
}



inline float binomial (int n, int k)
{
    float p = 1;
    for (int i = 1;  i <= k;  ++i)
        p *= float(n - (k-i)) / i;
    return p;
}


bool
ImageBufAlgo::make_kernel (ImageBuf &dst, const char *name,
                           float width, float height, float depth,
                           bool normalize)
{
    int w = std::max (1, (int)ceilf(width));
    int h = std::max (1, (int)ceilf(height));
    int d = std::max (1, (int)ceilf(depth));
    // Round up size to odd
    w |= 1;
    h |= 1;
    d |= 1;
    ImageSpec spec (w, h, 1 /*channels*/, TypeDesc::FLOAT);
    spec.depth = d;
    spec.x = -w/2;
    spec.y = -h/2;
    spec.z = -d/2;
    spec.full_x = spec.x;
    spec.full_y = spec.y;
    spec.full_z = spec.z;
    spec.full_width = spec.width;
    spec.full_height = spec.height;
    spec.full_depth = spec.depth;
    dst.alloc (spec);

    if (Filter2D *filter = Filter2D::create (name, width, height)) {
        // Named continuous filter from filter.h
        for (ImageBuf::Iterator<float> p (dst);  ! p.done();  ++p)
            p[0] = (*filter)((float)p.x(), (float)p.y());
        delete filter;
    } else if (!strcmp (name, "binomial")) {
        // Binomial filter
        float *wfilter = ALLOCA (float, width);
        for (int i = 0;  i < width;  ++i)
            wfilter[i] = binomial (width-1, i);
        float *hfilter = (height == width) ? wfilter : ALLOCA (float, height);
        if (height != width)
            for (int i = 0;  i < height;  ++i)
                hfilter[i] = binomial (height-1, i);
        float *dfilter = ALLOCA (float, depth);
        if (depth == 1)
            dfilter[0] = 1;
        else
            for (int i = 0;  i < depth;  ++i)
                dfilter[i] = binomial (depth-1, i);
        for (ImageBuf::Iterator<float> p (dst);  ! p.done();  ++p)
            p[0] = wfilter[p.x()-spec.x] * hfilter[p.y()-spec.y] * dfilter[p.z()-spec.z];
    } else {
        // No filter -- make a box
        float val = normalize ? 1.0f / ((w*h*d)) : 1.0f;
        for (ImageBuf::Iterator<float> p (dst);  ! p.done();  ++p)
            p[0] = val;
        dst.error ("Unknown kernel \"%s\"", name);
        return false;
    }
    if (normalize) {
        float sum = 0;
        for (ImageBuf::Iterator<float> p (dst);  ! p.done();  ++p)
            sum += p[0];
        for (ImageBuf::Iterator<float> p (dst);  ! p.done();  ++p)
            p[0] = p[0] / sum;
    }
    return true;
}



// Helper function for unsharp mask to perform the thresholding
static bool
threshold_to_zero (ImageBuf &dst, float threshold,
                   ROI roi, int nthreads)
{
    ASSERT (dst.spec().format.basetype == TypeDesc::FLOAT);

    if (nthreads != 1 && roi.npixels() >= 1000) {
        // Lots of pixels and request for multi threads? Parallelize.
        ImageBufAlgo::parallel_image (
            boost::bind(threshold_to_zero, boost::ref(dst), threshold,
                        _1 /*roi*/, 1 /*nthreads*/),
            roi, nthreads);
        return true;
    }

    // Serial case
    for (ImageBuf::Iterator<float> p (dst, roi);  ! p.done();  ++p)
        for (int c = roi.chbegin;  c < roi.chend;  ++c)
            if (fabsf(p[c]) < threshold)
                p[c] = 0.0f;

    return true;
}



bool
ImageBufAlgo::unsharp_mask (ImageBuf &dst, const ImageBuf &src,
                            const char *kernel, float width,
                            float contrast, float threshold,
                            ROI roi, int nthreads)
{
    if (! IBAprep (roi, &dst, &src,
            IBAprep_REQUIRE_SAME_NCHANNELS | IBAprep_NO_SUPPORT_VOLUME))
        return false;

    // Blur the source image, store in Blurry
    ImageBuf K;
    if (! make_kernel (K, kernel, width, width)) {
        dst.error ("%s", K.geterror());
        return false;
    }
    ImageSpec BlurrySpec = src.spec();
    BlurrySpec.set_format (TypeDesc::FLOAT);  // force float
    ImageBuf Blurry (BlurrySpec);
    if (! convolve (Blurry, src, K, true, roi, nthreads)) {
        dst.error ("%s", Blurry.geterror());
        return false;
    }

    // Compute the difference between the source image and the blurry
    // version.  (We store it in the same buffer we used for the difference
    // image.)
    ImageBuf &Diff (Blurry);
    bool ok = sub (Diff, src, Blurry, roi, nthreads);

    if (ok && threshold > 0.0f)
        ok = threshold_to_zero (Diff, threshold, roi, nthreads);

    // Scale the difference image by the contrast
    if (ok)
        ok = mul (Diff, contrast, roi, nthreads);
    if (! ok) {
        dst.error ("%s", Diff.geterror());
        return false;
    }

    // Add the scaled difference to the original, to get the final answer
    ok = add (dst, src, Diff, roi, nthreads);

    return ok;
}



// Helper function: fft of the horizontal rows
static bool
hfft_ (ImageBuf &dst, const ImageBuf &src, bool inverse, bool unitary,
       ROI roi, int nthreads)
{
    ASSERT (dst.spec().format.basetype == TypeDesc::FLOAT &&
            src.spec().format.basetype == TypeDesc::FLOAT &&
            dst.spec().nchannels == 2 && src.spec().nchannels == 2);

    if (nthreads != 1 && roi.npixels() >= 1000) {
        // Lots of pixels and request for multi threads? Parallelize.
        ImageBufAlgo::parallel_image (
            boost::bind (hfft_, boost::ref(dst), boost::cref(src),
                         inverse, unitary,
                         _1 /*roi*/, 1 /*nthreads*/),
            roi, nthreads);
        return true;
    }

    // Serial case
    int width = roi.width();
    float rescale = sqrtf (1.0f / width);
    kissfft<float> F (width, inverse);
    for (int z = roi.zbegin;  z < roi.zend;  ++z) {
        for (int y = roi.ybegin;  y < roi.yend;  ++y) {
            std::complex<float> *s, *d;
            s = (std::complex<float> *)src.pixeladdr(roi.xbegin, y, z);
            d = (std::complex<float> *)dst.pixeladdr(roi.xbegin, y, z);
            F.transform (s, d);
            if (unitary)
                for (int x = 0;  x < width;  ++x)
                    d[x] *= rescale;
        }
    }
    return true;
}



bool
ImageBufAlgo::fft (ImageBuf &dst, const ImageBuf &src,
                   ROI roi, int nthreads)
{
    if (src.spec().depth > 1) {
        dst.error ("ImageBufAlgo::fft does not support volume images");
        return false;
    }
    if (! roi.defined())
        roi = roi_union (get_roi (src.spec()), get_roi_full (src.spec()));
    roi.chend = roi.chbegin+1;   // One channel only

    // Construct a spec that describes the result
    ImageSpec spec = src.spec();
    spec.width = spec.full_width = roi.width();
    spec.height = spec.full_height = roi.height();
    spec.depth = spec.full_depth = 1;
    spec.x = spec.full_x = 0;
    spec.y = spec.full_y = 0;
    spec.z = spec.full_z = 0;
    spec.set_format (TypeDesc::FLOAT);
    spec.channelformats.clear();
    spec.nchannels = 2;
    spec.channelnames.clear();
    spec.channelnames.push_back ("real");
    spec.channelnames.push_back ("imag");

    // And a spec that describes the transposed intermediate
    ImageSpec specT = spec;
    std::swap (specT.width, specT.height);
    std::swap (specT.full_width, specT.full_height);

    // Resize dst
    dst.reset (dst.name(), spec);

    // Copy src to a 2-channel (for "complex") float buffer
    ImageBuf A (spec);   // zeros it out automatically
    if (! ImageBufAlgo::paste (A, 0, 0, 0, 0, src, roi, nthreads)) {
        dst.error ("%s", A.geterror());
        return false;
    }

    // FFT the rows (into temp buffer B).
    ImageBuf B (spec);
    hfft_ (B, A, false /*inverse*/, true /*unitary*/,
           get_roi(B.spec()), nthreads);

    // Transpose and shift back to A
    A.clear ();
    ImageBufAlgo::transpose (A, B, ROI::All(), nthreads);

    // FFT what was originally the columns (back to B)
    B.reset (specT);
    hfft_ (B, A, false /*inverse*/, true /*unitary*/,
           get_roi(A.spec()), nthreads);

    // Transpose again, into the dest
    ImageBufAlgo::transpose (dst, B, ROI::All(), nthreads);

    return true;
}



bool
ImageBufAlgo::ifft (ImageBuf &dst, const ImageBuf &src,
                    ROI roi, int nthreads)
{
    if (src.nchannels() != 2 || src.spec().format != TypeDesc::FLOAT) {
        dst.error ("ifft can only be done on 2-channel float images");
        return false;
    }
    if (src.spec().depth > 1) {
        dst.error ("ImageBufAlgo::ifft does not support volume images");
        return false;
    }

    if (! roi.defined())
        roi = roi_union (get_roi (src.spec()), get_roi_full (src.spec()));
    roi.chbegin = 0;
    roi.chend = 2;

    // Construct a spec that describes the result
    ImageSpec spec = src.spec();
    spec.width = spec.full_width = roi.width();
    spec.height = spec.full_height = roi.height();
    spec.depth = spec.full_depth = 1;
    spec.x = spec.full_x = 0;
    spec.y = spec.full_y = 0;
    spec.z = spec.full_z = 0;
    spec.set_format (TypeDesc::FLOAT);
    spec.channelformats.clear();
    spec.nchannels = 2;
    spec.channelnames.clear();
    spec.channelnames.push_back ("real");
    spec.channelnames.push_back ("imag");

    // Inverse FFT the rows (into temp buffer B).
    ImageBuf B (spec);
    hfft_ (B, src, true /*inverse*/, true /*unitary*/,
           get_roi(B.spec()), nthreads);

    // Transpose and shift back to A
    ImageBuf A;
    ImageBufAlgo::transpose (A, B, ROI::All(), nthreads);

    // Inverse FFT what was originally the columns (back to B)
    B.reset (A.spec());
    hfft_ (B, A, true /*inverse*/, true /*unitary*/,
           get_roi(A.spec()), nthreads);

    // Transpose again, into the dst, in the process throw out the
    // imaginary part and go back to a single (real) channel.
    spec.nchannels = 1;
    spec.channelnames.clear ();
    spec.channelnames.push_back ("R");
    dst.reset (dst.name(), spec);
    ROI Broi = get_roi(B.spec());
    Broi.chend = 1;
    ImageBufAlgo::transpose (dst, B, Broi, nthreads);

    return true;
}



template<class Rtype, class Atype>
static bool
polar_to_complex_impl (ImageBuf &R, const ImageBuf &A, ROI roi, int nthreads)
{
    if (nthreads != 1 && roi.npixels() >= 1000) {
        // Possible multiple thread case -- recurse via parallel_image
        ImageBufAlgo::parallel_image (
            boost::bind(polar_to_complex_impl<Rtype,Atype>, boost::ref(R), boost::cref(A),
                        _1 /*roi*/, 1 /*nthreads*/),
            roi, nthreads);
        return true;
    }

    ImageBuf::ConstIterator<Atype> a (A, roi);
    for (ImageBuf::Iterator<Rtype> r (R, roi);  !r.done();  ++r, ++a) {
        float amp = a[0];
        float phase = a[1];
        float sine, cosine;
        sincos (phase, &sine, &cosine);
        r[0] = amp * cosine;
        r[1] = amp * sine;
    }
    return true;
}



template<class Rtype, class Atype>
static bool
complex_to_polar_impl (ImageBuf &R, const ImageBuf &A, ROI roi, int nthreads)
{
    if (nthreads != 1 && roi.npixels() >= 1000) {
        // Possible multiple thread case -- recurse via parallel_image
        ImageBufAlgo::parallel_image (
            boost::bind(complex_to_polar_impl<Rtype,Atype>, boost::ref(R), boost::cref(A),
                        _1 /*roi*/, 1 /*nthreads*/),
            roi, nthreads);
        return true;
    }

    ImageBuf::ConstIterator<Atype> a (A, roi);
    for (ImageBuf::Iterator<Rtype> r (R, roi);  !r.done();  ++r, ++a) {
        float real = a[0];
        float imag = a[1];
        float phase = std::atan2 (imag, real);
        if (phase < 0.0f)
            phase += float (M_TWO_PI);
        r[0] = hypotf (real, imag);
        r[1] = phase;
    }
    return true;
}



bool
ImageBufAlgo::polar_to_complex (ImageBuf &dst, const ImageBuf &src,
                             ROI roi, int nthreads)
{
    if (src.nchannels() != 2) {
        dst.error ("polar_to_complex can only be done on 2-channel");
        return false;
    }

    if (! IBAprep (roi, &dst, &src))
        return false;
    if (dst.nchannels() != 2) {
        dst.error ("polar_to_complex can only be done on 2-channel");
        return false;
    }
    bool ok;
    OIIO_DISPATCH_COMMON_TYPES2 (ok, "polar_to_complex",
                                 polar_to_complex_impl, dst.spec().format,
                                 src.spec().format, dst, src, roi, nthreads);
    return ok;
}




bool
ImageBufAlgo::complex_to_polar (ImageBuf &dst, const ImageBuf &src,
                             ROI roi, int nthreads)
{
    if (src.nchannels() != 2) {
        dst.error ("complex_to_polar can only be done on 2-channel");
        return false;
    }

    if (! IBAprep (roi, &dst, &src))
        return false;
    if (dst.nchannels() != 2) {
        dst.error ("complex_to_polar can only be done on 2-channel");
        return false;
    }
    bool ok;
    OIIO_DISPATCH_COMMON_TYPES2 (ok, "complex_to_polar",
                                 complex_to_polar_impl, dst.spec().format,
                                 src.spec().format, dst, src, roi, nthreads);
    return ok;
}




#ifdef USE_FREETYPE
namespace { // anon
static mutex ft_mutex;
static FT_Library ft_library = NULL;
static bool ft_broken = false;
static const char * default_font_name[] = {
        "cour", "Courier New", "FreeMono", NULL
     };
} // anon namespace
#endif


bool
ImageBufAlgo::render_text (ImageBuf &R, int x, int y, const std::string &text,
                           int fontsize, const std::string &font_,
                           const float *textcolor)
{
    if (R.spec().depth > 1) {
        R.error ("ImageBufAlgo::render_text does not support volume images");
        return false;
    }

#ifdef USE_FREETYPE
    // If we know FT is broken, don't bother trying again
    if (ft_broken)
        return false;

    // Thread safety
    lock_guard ft_lock (ft_mutex);
    int error = 0;

    // If FT not yet initialized, do it now.
    if (! ft_library) {
        error = FT_Init_FreeType (&ft_library);
        if (error) {
            ft_broken = true;
            R.error ("Could not initialize FreeType for font rendering");
            return false;
        }
    }

    // A set of likely directories for fonts to live, across several systems.
    std::vector<std::string> search_dirs;
    const char *home = getenv ("HOME");
    if (home && *home) {
        std::string h (home);
        search_dirs.push_back (h + "/fonts");
        search_dirs.push_back (h + "/Fonts");
        search_dirs.push_back (h + "/Library/Fonts");
    }
    const char *systemRoot = getenv ("SystemRoot");
    if (systemRoot && *systemRoot) {
        std::string sysroot (systemRoot);
        search_dirs.push_back (sysroot + "/Fonts");
    }
    search_dirs.push_back ("/usr/share/fonts");
    search_dirs.push_back ("/Library/Fonts");
    search_dirs.push_back ("C:/Windows/Fonts");
    search_dirs.push_back ("/opt/local/share/fonts");

    // Try to find the font.  Experiment with several extensions
    std::string font = font_;
    if (font.empty()) {
        // nothing specified -- look for something to use as a default.
        for (int j = 0;  default_font_name[j] && font.empty(); ++j) {
            static const char *extensions[] = { "", ".ttf", ".pfa", ".pfb", NULL };
            for (int i = 0;  font.empty() && extensions[i];  ++i)
                font = Filesystem::searchpath_find (std::string(default_font_name[j])+extensions[i],
                                                 search_dirs, true, true);
        }
        if (font.empty()) {
            R.error ("Could not set default font face");
            return false;
        }
    } else if (Filesystem::is_regular (font)) {
        // directly specified a filename -- use it
    } else {
        // A font name was specified but it's not a full path, look for it
        std::string f;
        static const char *extensions[] = { "", ".ttf", ".pfa", ".pfb", NULL };
        for (int i = 0;  f.empty() && extensions[i];  ++i)
            f = Filesystem::searchpath_find (font+extensions[i],
                                             search_dirs, true, true);
        if (f.empty()) {
            R.error ("Could not set font face to \"%s\"", font);
            return false;
        }
        font = f;
    }

    ASSERT (! font.empty());
    if (! Filesystem::is_regular (font)) {
        R.error ("Could not find font \"%s\"", font);
        return false;
    }

    FT_Face face;      // handle to face object
    error = FT_New_Face (ft_library, font.c_str(), 0 /* face index */, &face);
    if (error) {
        R.error ("Could not set font face to \"%s\"", font);
        return false;  // couldn't open the face
    }

    error = FT_Set_Pixel_Sizes (face,        // handle to face object
                                0,           // pixel_width
                                fontsize);   // pixel_heigh
    if (error) {
        FT_Done_Face (face);
        R.error ("Could not set font size to %d", fontsize);
        return false;  // couldn't set the character size
    }

    FT_GlyphSlot slot = face->glyph;  // a small shortcut
    int nchannels = R.spec().nchannels;
    float *pixelcolor = ALLOCA (float, nchannels);
    if (! textcolor) {
        float *localtextcolor = ALLOCA (float, nchannels);
        for (int c = 0;  c < nchannels;  ++c)
            localtextcolor[c] = 1.0f;
        textcolor = localtextcolor;
    }

    for (size_t n = 0, e = text.size();  n < e;  ++n) {
        // load glyph image into the slot (erase previous one)
        error = FT_Load_Char (face, text[n], FT_LOAD_RENDER);
        if (error)
            continue;  // ignore errors
        // now, draw to our target surface
        for (int j = 0;  j < slot->bitmap.rows; ++j) {
            int ry = y + j - slot->bitmap_top;
            for (int i = 0;  i < slot->bitmap.width; ++i) {
                int rx = x + i + slot->bitmap_left;
                float b = slot->bitmap.buffer[slot->bitmap.pitch*j+i] / 255.0f;
                R.getpixel (rx, ry, pixelcolor);
                for (int c = 0;  c < nchannels;  ++c)
                    pixelcolor[c] = b*textcolor[c] + (1.0f-b) * pixelcolor[c];
                R.setpixel (rx, ry, pixelcolor);
            }
        }
        // increment pen position
        x += slot->advance.x >> 6;
    }

    FT_Done_Face (face);
    return true;

#else
    R.error ("OpenImageIO was not compiled with FreeType for font rendering");
    return false;   // Font rendering not supported
#endif
}



// Helper for fillholes_pp: for any nonzero alpha pixels in dst, divide
// all components by alpha.
static bool
divide_by_alpha (ImageBuf &dst, ROI roi, int nthreads)
{
    if (nthreads != 1 && roi.npixels() >= 1000) {
        // Lots of pixels and request for multi threads? Parallelize.
        ImageBufAlgo::parallel_image (
            boost::bind(divide_by_alpha, boost::ref(dst),
                        _1 /*roi*/, 1 /*nthreads*/),
            roi, nthreads);
        return true;
    }

    // Serial case
    const ImageSpec &spec (dst.spec());
    ASSERT (spec.format == TypeDesc::FLOAT);
    int nc = spec.nchannels;
    int ac = spec.alpha_channel;
    for (ImageBuf::Iterator<float> d (dst, roi);  ! d.done();  ++d) {
        float alpha = d[ac];
        if (alpha != 0.0f) {
            for (int c = 0; c < nc; ++c)
                d[c] = d[c] / alpha;
        }
    }
    return true;
}



bool
ImageBufAlgo::fillholes_pushpull (ImageBuf &dst, const ImageBuf &src,
                                  ROI roi, int nthreads)
{
    if (! IBAprep (roi, &dst, &src))
        return false;
    const ImageSpec &dstspec (dst.spec());
    if (dstspec.nchannels != src.nchannels()) {
        dst.error ("channel number mismatch: %d vs. %d", 
                   dstspec.nchannels, src.spec().nchannels);
        return false;
    }
    if (dst.spec().depth > 1 || src.spec().depth > 1) {
        dst.error ("ImageBufAlgo::fillholes_pushpull does not support volume images");
        return false;
    }
    if (dstspec.alpha_channel < 0 ||
        dstspec.alpha_channel != src.spec().alpha_channel) {
        dst.error ("Must have alpha channels");
        return false;
    }

    // We generate a bunch of temp images to form an image pyramid.
    // These give us a place to stash them and make sure they are
    // auto-deleted when the function exits.
    std::vector<boost::shared_ptr<ImageBuf> > pyramid;

    // First, make a writeable copy of the original image (converting
    // to float as a convenience) as the top level of the pyramid.
    ImageSpec topspec = src.spec();
    topspec.set_format (TypeDesc::FLOAT);
    ImageBuf *top = new ImageBuf (topspec);
    paste (*top, topspec.x, topspec.y, topspec.z, 0, src);
    pyramid.push_back (boost::shared_ptr<ImageBuf>(top));

    // Construct the rest of the pyramid by successive x/2 resizing and
    // then dividing nonzero alpha pixels by their alpha (this "spreads
    // out" the defined part of the image).
    int w = src.spec().width, h = src.spec().height;
    while (w > 1 || h > 1) {
        w = std::max (1, w/2);
        h = std::max (1, h/2);
        ImageSpec smallspec (w, h, src.nchannels(), TypeDesc::FLOAT);
        ImageBuf *small = new ImageBuf (smallspec);
        ImageBufAlgo::resize (*small, *pyramid.back(), "triangle");
        divide_by_alpha (*small, get_roi(smallspec), nthreads);
        pyramid.push_back (boost::shared_ptr<ImageBuf>(small));
        //debug small->save();
    }

    // Now pull back up the pyramid by doing an alpha composite of level
    // i over a resized level i+1, thus filling in the alpha holes.  By
    // time we get to the top, pixels whose original alpha are
    // unchanged, those with alpha < 1 are replaced by the blended
    // colors of the higher pyramid levels.
    for (int i = (int)pyramid.size()-2;  i >= 0;  --i) {
        ImageBuf &big(*pyramid[i]), &small(*pyramid[i+1]);
        ImageBuf blowup (big.spec());
        ImageBufAlgo::resize (blowup, small, "triangle");
        ImageBufAlgo::over (big, big, blowup);
        //debug big.save (Strutil::format ("after%d.exr", i));
    }

    // Now copy the completed base layer of the pyramid back to the
    // original requested output.
    paste (dst, dstspec.x, dstspec.y, dstspec.z, 0, *pyramid[0]);

    return true;
}


}
OIIO_NAMESPACE_EXIT
