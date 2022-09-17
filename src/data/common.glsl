vec3 toNonlinear(vec3 linear) {
	vec3 a = step(vec3(0.0031308), linear);
	vec3 clin = 12.92 * linear;
	vec3 cgamma = 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055;
	return mix(clin, cgamma, a);
}

vec3 toLinear(vec3 nonlinear) {
	vec3 a = step(vec3(0.04045), nonlinear);
	vec3 clin = nonlinear / 12.92;
	vec3 cgamma = pow((nonlinear + 0.055) / 1.055, vec3(2.4));
	return mix(clin, cgamma, a);
}

vec3 toNonlinearOpt(vec3 col, bool needed) {
	return needed ? toNonlinear(col) : col;
}

vec3 toLinearOpt(vec3 col, bool needed) {
	return needed ? toLinear(col) : col;
}

vec4 toNonlinearOpt(vec4 col, bool needed) {
	return needed ? vec4(toNonlinear(col.rgb), col.a) : col;
}

vec4 toLinearOpt(vec4 col, bool needed) {
	return needed ? vec4(toLinear(col.rgb), col.a) : col;
}
