#ifndef EDITOR_RESTART_SETTING
#error missing_editor_restart_setting
#endif

struct Number { float value; };
float shade(float value) { return value; }
float4 Main(float x : TEXCOORD0) : SV_Target { return shade(x).xxxx; }
