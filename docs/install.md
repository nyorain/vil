# Installing on windows

On windows, layers are installed via registry entries, you have to add
a registry entry in `HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\Vulkan\ExplicitLayers` pointing to the generated layer config 
json (that must be located in the same folder as `VkLayer_live_introspection.dll`).
See [here](https://asawicki.info/news_1683_vulkan_layers_dont_work_look_at_registry.html) or [here](https://vulkan.lunarg.com/doc/view/1.1.121.1/linux/layer_configuration.html) for more details.
You can simply run the `register_layer.bat` script in the build directory. Note that it will require admin privileges
to add the registry key. You should usually not run random batch scripts from the internet that require admin privileges,
so feel free to do it manually in an admin prompt:

```
REG ADD HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\Vulkan\ExplicitLayers /v <filepath of VkLayer_live_introspection.json> /t REG_DWORD /d 0
```

Replace `<filepath of VkLayer_live_introspection.json>` with full file path of the generated `VkLayer_live_introspection.json` file, e.g. `D:\code\vil\build\vs19\VkLayer_live_introspection.json`.

When building with something else than MSVC, you might have to adjust the name of the dll. For instance, MinGW might add a `lib` prefix. In that case, when using `vil_api.h`, you'll also have to redefine `VIL_LIB_NAME` before including the header to the name of the dll.
