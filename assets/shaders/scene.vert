#version 450 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
uniform mat4 uModel;
uniform mat4 uViewProj;
uniform float iTime;                 // same name as in effects, so a vertex stage can animate
out vec3 vWorld;
out vec3 vNormal;
out vec3 vObj;
out vec2 vUv;
/*GLUE_USER_DECLS*/
void main() {
    vec3 p = aPos;
    vec3 n = aNormal;
    /*GLUE_USER_CALL*/
    vec4 w = uModel * vec4(p, 1.0);
    vWorld = w.xyz;
    vNormal = mat3(uModel) * n;      // uniform scale only, so no inverse-transpose needed
    vObj = aPos;                     // the checker stays attached to the undisplaced surface
    vUv = aUv;
    gl_Position = uViewProj * w;
}
