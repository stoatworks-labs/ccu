#pragma once

#include <FFGLSDK.h>

namespace ccu
{
/**
    An off-screen buffer for one stage of the chain.

    Four things on top of the SDK's FFGLFBO.

    **It reallocates only when it has to.** Ensure() is called every frame and
    is a no-op in the overwhelming majority of them; it reallocates when the
    host's picture changes size. A reallocation clears, which is safe here
    because nothing on the GPU outlives a frame: the one buffer is the linear
    picture, rebuilt from the host's every frame (see `Ccu.h`). The only state
    across frames is the white-balance drift, and that is a CPU double.

    **It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
    deletes the framebuffer and the depth renderbuffer, then tests
    `depthBufferID` a second time where it plainly meant `colorTextureID` --
    so the colour texture is leaked on every release (SDK b1afaf9,
    `FFGLFBO.cpp`). `Destroy()` deletes it first.

    **It owns its filtering.** The linear buffer is read texel for texel with
    `texelFetch` -- the detail taps do their own lerp between two fetches at
    a fractional spacing, so nothing rests on a texture unit's filtering --
    and is therefore Nearest. Linear and Mipmapped are tinsel's and unused.

    Copied from tinsel (by way of clamp and toner), where it was written
    for a glow chain; the mechanism is unchanged.

    **It has no depth buffer.** `FFGLFBO::Initialise()` always attaches a
    24-bit depth renderbuffer the size of the picture, and nothing here draws
    with depth: at 4K that is about 32 MB a buffer for nothing. Ensure()
    allocates through InitialiseColourOnly() instead, the same steps with the
    colour texture alone.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	enum class Sampling
	{
		Nearest,  ///< for data read texel-for-texel. No filtering, no mip chain.
		Linear,   ///< for pictures read between texels. Bilinear, no mip chain.
		Mipmapped ///< for pictures that also get reduced. Trilinear + GenerateMipmaps().
	};

	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared: a buffer whose
	/// contents are undefined is not "a bit of noise on the first frame", it is
	/// whatever texture memory the driver handed back -- and for the edge
	/// history buffers, which feed back into themselves, it is noise that never
	/// washes out.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format, Sampling sampling );

	/// Rebuild the mip chain from level 0. Call after rendering into a
	/// Sampling::Mipmapped buffer and before anything samples it; a stale chain
	/// does not look like an error, it looks like the wrong footage.
	void GenerateMipmaps();

	/// Highest mip level this buffer has, i.e. the 1x1 one. The centroid pass
	/// needs it as a uniform: `textureQueryLevels` is GLSL 4.30 and these
	/// shaders are 4.10.
	float MaxMipLevel() const;

	/// Clear to transparent black. The edge history needs this when the effect
	/// is re-enabled or the source changes size, so the first stabilised frame
	/// blends against nothing rather than against the last clip.
	void Clear();

	/// The colour texture, for binding as an input to a later pass.
	///
	/// The SDK keeps `colorTextureID` protected and offers only
	/// `GetTextureInfo()`, which builds and returns an `FFGLTextureStruct` --
	/// six fields assembled to reach one of them, at every bind of every pass
	/// of every frame. A subclass can just say which texture it is.
	GLuint TextureID() const
	{
		return colorTextureID;
	}

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	bool IsValid() const
	{
		return GetGLID() != 0;
	}

private:
	/// FFGLFBO::Initialise() less the depth renderbuffer: the framebuffer, the
	/// colour texture and its attachment, and everything released again if
	/// the framebuffer is not complete.
	bool InitialiseColourOnly( GLsizei requestedWidth, GLsizei requestedHeight, GLint format );

	Sampling sampling = Sampling::Nearest;
};

} // namespace ccu
