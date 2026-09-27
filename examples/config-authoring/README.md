# Configuration authoring tutorial workspace

Open this directory as an independent VS Code workspace, or copy it beside a
Visual Studio solution file. There is deliberately no `shadertoolsconfig.json`
here: create it with the editor's guided authoring tool, then add the named
pixel shader variants described in the
[configuration authoring tutorial](../../docs/tutorials/configuration-authoring.md).

`Shaders/Fullscreen.hlsl` has the `MainVS` vertex entry point;
`Shaders/PostProcess.hlsl` has the `MainPS` pixel entry point and includes
`Tint.hlsli`. Neither file needs a separate compiler or engine installation to
be discovered by HLSL-LSP's bundled DXC runtime.
