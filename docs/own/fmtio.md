# A new, improved format IO interface

```
// Will interpret 'src' as one block of data in the given 'format', containing
// the only given 'aspect' (for depth/stencil images). Returns all the texels
// in the block, row-major.
span<Vec4d> read(VkFormat format, VkImageAspectBits aspect, 
	span<const std::byte>& src, ThreadMemScope& ms);

// Alternate version preserving integers.
span<Vec4i64> readi(VkFormat format, VkImageAspectBits aspect, 
	span<const std::byte>& src, ThreadMemScope& ms);
```

Implementation:
- For BC textures, https://github.com/richgel999/bc7enc looks promising
- ASTC and ETC will be more complicated. But they are mainly present on
  mobile, vil isn't ready for that yet anyways

'aspect' is somewhat overkill, right?
Maybe just return x=depth, y=stencil for depth/stencil formats?
not sure what about multiplanar formats
