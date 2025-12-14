# Feature support overview

Even when a feature/extension isn't explicitly supported by the layer,
there are good chances that it might just work. If you plan on using
extensions not explicitly supported, make sure to set the `VIL_WRAP=0`
environment variable, it will significantly increase the chance that
new extensions "just work", with a slight performance hit.
See [env.md](env.md) for more details.

For a list of supported/unsupported extensions, see [exts.hpp](../src/exts.hpp).

## NOTES:
- vendor-specific extensions are generally not a focus, unless someone
  is interested in them and writes the code or sponsors someone to do it.
- the layer does not have support for VK_EXT_debug_report/VK_EXT_debug_marker
  and will likely not add it in the future, either, as it is deprecated.
  Just use debug_utils, it's a better replacement.
- support VK_KHR_device_group in the near future is unlikely since making it
  work with our concepts is not trivial
- support VK_EXT_transform_feedback in the near future is unlikely as the
  layer itself is using it for the vertex viewer (which would conflict with
  an application-side use of it). It's only supposed to be used by tools/layers
  anyways.
- Extensions that have significant changes to command recording (such as
  new pipeline bind points or new commands) will very likely always lead to
  significant problems. Implementing basic support for them isn't complicated
  though
