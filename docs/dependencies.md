# Dependencies

- SPIRV-Cross: we use a slightly modified version with utilties for shader
  patching. General patches should be contribued back upstream to minimize
  rebasing effort.
- SPIRV-Tools: currently needed only for shader disassembly.
  We currently just rely on the system dependency.
  TODO: include (cmake?) subproject that is optionally built.
- dlg: Used for logging, always built locally. Git submodule
- nytl: Various C++ utilities, header-only. Git submodule
- swa: Needed to create a window on windows and linux. Git submodule.
- imgio: Only needed for image I/O.
  TODO: make optional. Should not be needed/included in default builds
- Tracy: currently included in the repositorty.
  TODO: make submodule? And re-evalutate if it's really the best tool for the job.
