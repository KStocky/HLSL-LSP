# Configuration authoring tutorial workspace

Open this directory as an independent VS Code workspace, or copy it beside a
Visual Studio solution file. There is deliberately no `shadertoolsconfig.json`
here: create it with the editor's guided authoring tool, then add the named
pixel shader variants described in the
[configuration authoring tutorial](../../docs/tutorials/configuration-authoring.md).
To exercise the opt-in SDK with this same shader instead, follow the
[runtime capture tutorial](../../docs/tutorials/runtime-capture.md).

`Shaders/Fullscreen.hlsl` has the `MainVS` vertex entry point;
`Shaders/PostProcess.hlsl` has the `MainPS` pixel entry point and includes
`Tint.hlsli`. Neither file needs a separate compiler or engine installation to
be discovered by HLSL-LSP's bundled DXC runtime.
