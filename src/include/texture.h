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
/// An API for accessing filtered texture lookups via a system that
/// automatically manages a cache of resident texture.


#ifndef TEXTURE_H
#define TEXTURE_H

#include "varyingref.h"
#include "ustring.h"

namespace Imath {
#ifndef INCLUDED_IMATHVEC_H
    class V3f;
#endif
#ifndef INCLUDED_IMATHMATRIX_H
    class M44f;
#endif
};


namespace OpenImageIO {

// Forward declaration
namespace pvt {
    class TextureSystemImpl;
};


/// Data type for flags that indicate on a point-by-point basis whether
/// we want computations to be performed.
typedef unsigned char Runflag;

/// Pre-defined values for Runflag's.
///
enum RunFlagVal { RunFlagOff = 0, RunFlagOn = 255 };



/// Encapsulate all the options needed for texture lookups.  Making
/// these options all separate parameters to the texture API routines is
/// very ugly and also a big pain whenever we think of new options to
/// add.  So instead we collect all those little options into one
/// structure that can just be passed by reference to the texture API
/// routines.
class TextureOptions {
public:
    /// Wrap mode describes what happens when texture coordinates describe
    /// a value outside the usual [0,1] range where a texture is defined.
    enum Wrap {
        WrapDefault,        ///< Use the default found in the file
        WrapBlack,          ///< Black outside [0..1]
        WrapClamp,          ///< Clamp to [0..1]
        WrapPeriodic,       ///< Periodic mod 1
        WrapMirror,         ///< Mirror the image
        WrapLast            ///< Mark the end -- don't use this!
    };

    /// Create a TextureOptions with all fields initialized to reasonable
    /// defaults.
    TextureOptions ();

    // Options that must be the same for all points we're texturing at once
    int firstchannel;       ///< First channel of the lookup
    int nchannels;          ///< Number of channels to look up: 1 or 3
    Wrap swrap;             ///< Wrap mode in the s direction
    Wrap twrap;             ///< Wrap mode in the t direction

    // Options that may be different for each point we're texturing
    VaryingRef<float> sblur, tblur;   ///< Blur amount
    VaryingRef<float> swidth, twidth; ///< Multiplier for derivatives
    VaryingRef<float> bias;           ///< Bias
    VaryingRef<float> fill;           ///< Fill value for missing channels
    VaryingRef<int>   samples;        ///< Number of samples

    // For 3D volume texture lookups only:
    Wrap zwrap;                ///< Wrap mode in the z direction
    VaryingRef<float> zblur;   ///< Blur amount in the z direction
    VaryingRef<float> zwidth;  ///< Multiplier for derivatives in z direction

    // Storage for results
    VaryingRef<float> alpha;   ///< If non-null put the alpha channel here

    /// Utility: Return the Wrap enum corresponding to a wrap name:
    /// "default", "black", "clamp", "periodic", "mirror".
    static Wrap decode_wrapmode (const char *name);

    /// Utility: Parse a single wrap mode (e.g., "periodic") or a
    /// comma-separated wrap modes string (e.g., "black,clamp") into
    /// separate Wrap enums for s and t.
    static void parse_wrapmodes (const char *wrapmodes,
                                 TextureOptions::Wrap &swrapcode,
                                 TextureOptions::Wrap &twrapcode);
    

    /// Special private ctr that makes a canonical default TextureOptions.
    /// For use internal to libtexture.  Users, don't call this!
    /// Though, there is no harm.  It's just not as efficient as the
    /// default ctr that memcpy's a canonical pre-constructed default.
    TextureOptions (bool);

private:
    // Options set INTERNALLY by libtexture after the options are passed
    // by the user.  Users should not attempt to alter these!
    bool stateful;         // False for a new-ish TextureOptions
    int actualchannels;    // True number of channels read
    typedef bool (*wrap_impl) (int &coord, int width);
    wrap_impl swrap_func, twrap_func;
    friend class OpenImageIO::pvt::TextureSystemImpl;
};



/// Define an API to an abstract class that that manages texture files,
/// caches of open file handles as well as tiles of texels so that truly
/// huge amounts of texture may be accessed by an application with low
/// memory footprint, and ways to perform antialiased texture, shadow
/// map, and environment map lookups.
class TextureSystem {
public:
    /// Creat a TextureSystem and return a pointer.  This should only be
    /// freed by passing it to TextureSystem::destroy()!
    static TextureSystem *create ();

    /// Destroy a TextureSystem that was created using
    /// TextureSystem::create().
    static void destroy (TextureSystem * &x);

    TextureSystem (void) { }
    virtual ~TextureSystem () { }

    /// Close everything, free resources, start from scratch.
    ///
    virtual void clear () = 0;

    // Set options
    virtual void max_open_files (int nfiles) = 0;
    virtual void max_memory_MB (float size) = 0;
    virtual void searchpath (const std::string &path) = 0;
    virtual void worldtocommon (const float *mx) = 0;
    void worldtocommon (const Imath::M44f &w2c) {
        worldtocommon ((const float *)&w2c);
    }

    // Retrieve options
    virtual int max_open_files () const = 0;
    virtual float max_memory_MB () const = 0;
    virtual std::string searchpath () const = 0;

    /// Filtered 2D texture lookup for a single point.
    ///
    /// s,t are the texture coordinates; dsdx, dtdx, dsdy, and dtdy are
    /// the differentials of s and t change in some canonical directions
    /// x and y.  The choice of x and y are not important to the
    /// implementation; it can be any imposed 2D coordinates, such as
    /// pixels in screen space, adjacent samples in parameter space on a
    /// surface, etc.
    ///
    /// Return true if the file is found and could be opened by an
    /// available ImageIO plugin, otherwise return false.
    virtual bool texture (ustring filename, TextureOptions &options,
                          float s, float t, float dsdx, float dtdx,
                          float dsdy, float dtdy, float *result) = 0;

    /// Retrieve filtered (possibly anisotropic) texture lookups for
    /// several points at once.
    ///
    /// All of the VaryingRef parameters (and fields in options)
    /// describe texture lookup parameters at an array of positions.
    /// But this routine only computes them from indices i where
    /// firstactive <= i <= lastactive, and ONLY when runflags[i] is
    /// nonzero.
    ///
    /// Return true if the file is found and could be opened by an
    /// available ImageIO plugin, otherwise return false.
    virtual bool texture (ustring filename, TextureOptions &options,
                          Runflag *runflags, int firstactive, int lastactive,
                          VaryingRef<float> s, VaryingRef<float> t,
                          VaryingRef<float> dsdx, VaryingRef<float> dtdx,
                          VaryingRef<float> dsdy, VaryingRef<float> dtdy,
                          float *result) = 0;

    /// Retrieve a 3D texture lookup at a single point.
    ///
    /// Return true if the file is found and could be opened by an
    /// available ImageIO plugin, otherwise return false.
    virtual bool texture (ustring filename, TextureOptions &options,
                          const Imath::V3f &P,
                          const Imath::V3f &dPdx, const Imath::V3f &dPdy,
                          float *result) = 0;

    /// Retrieve a 3D texture lookup at many points at once.
    ///
    /// Return true if the file is found and could be opened by an
    /// available ImageIO plugin, otherwise return false.
    virtual bool texture (ustring filename, TextureOptions &options,
                          Runflag *runflags, int firstactive, int lastactive,
                          VaryingRef<Imath::V3f> P,
                          VaryingRef<Imath::V3f> dPdx,
                          VaryingRef<Imath::V3f> dPdy,
                          float *result) = 0;

    /// Retrieve a shadow lookup for a single position P.
    ///
    /// Return true if the file is found and could be opened by an
    /// available ImageIO plugin, otherwise return false.
    virtual bool shadow (ustring filename, TextureOptions &options,
                         const Imath::V3f &P, const Imath::V3f &dPdx,
                         const Imath::V3f &dPdy, float *result) = 0;

    /// Retrieve a shadow lookup for position P at many points at once.
    ///
    /// Return true if the file is found and could be opened by an
    /// available ImageIO plugin, otherwise return false.
    virtual bool shadow (ustring filename, TextureOptions &options,
                         Runflag *runflags, int firstactive, int lastactive,
                         VaryingRef<Imath::V3f> P,
                         VaryingRef<Imath::V3f> dPdx,
                         VaryingRef<Imath::V3f> dPdy,
                         float *result) = 0;

    /// Retrieve an environment map lookup for direction R.
    ///
    /// Return true if the file is found and could be opened by an
    /// available ImageIO plugin, otherwise return false.
    virtual bool environment (ustring filename, TextureOptions &options,
                              const Imath::V3f &R, const Imath::V3f &dRdx,
                              const Imath::V3f &dRdy, float *result) = 0;

    /// Retrieve an environment map lookup for direction R, for many
    /// points at once.
    ///
    /// Return true if the file is found and could be opened by an
    /// available ImageIO plugin, otherwise return false.
    virtual bool environment (ustring filename, TextureOptions &options,
                              Runflag *runflags, int firstactive, int lastactive,
                              VaryingRef<Imath::V3f> R,
                              VaryingRef<Imath::V3f> dRdx,
                              VaryingRef<Imath::V3f> dRdy,
                              float *result) = 0;

    /// Get information about the given texture.  Return true if found
    /// and the data has been put in *data.  Return false if the texture
    /// doesn't exist, doesn't have the requested data, if the data
    /// doesn't match the type requested. or some other failure.
    virtual bool get_texture_info (ustring filename, ustring dataname,
                                   TypeDesc datatype, void *data) = 0;
    
    /// Get the ImageSpec associated with the named texture
    /// (specifically, the first MIP-map level).  If the file is found
    /// and is an image format that can be read, store a copy of its
    /// specification in spec and return true.  Return false if the file
    /// was not found or could not be opened as an image file by any
    /// available ImageIO plugin.
    virtual bool get_imagespec (ustring filename, ImageSpec &spec) = 0;

    /// Retrieve the rectangle of raw unfiltered texels spanning
    /// [xmin..xmax X ymin..ymax X zmin..zmax] (inclusive, specified as
    /// integer pixel coordinates), at the named MIP-map level, storing
    /// the texel values beginning at the address specified by result.
    /// The texel values will be converted to the type specified by
    /// format.  It is up to the caller to ensure that result points to
    /// an area of memory big enough to accommodate the requested
    /// rectangle (taking into consideration its dimensions, number of
    /// channels, and data format).
    ///
    /// Return true if the file is found and could be opened by an
    /// available ImageIO plugin, otherwise return false.
    virtual bool get_texels (ustring filename, TextureOptions &options,
                             int xmin, int xmax, int ymin, int ymax,
                             int zmin, int zmax, int level,
                             TypeDesc format, void *result) = 0;

    /// If any of the API routines returned false indicating an error,
    /// this routine will return the error string (and clear any error
    /// flags).  If no error has occurred since the last time geterror()
    /// was called, it will return an empty string.
    virtual std::string geterror () const = 0;

};


};  // end namespace OpenImageIO


#endif // TEXTURE_H
