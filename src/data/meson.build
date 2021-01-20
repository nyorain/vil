# needed shaders
spv = []
glslang = find_program('glslangValidator', required: false)

if glslang.found()
	overlay_shaders = {
		'gui.frag': { '': [] },
		'gui.vert': { '': [] },
		'image.frag': {
			'1DArray.': ['-DTEX_TYPE_1D_ARRAY'],
			'2DArray.': ['-DTEX_TYPE_2D_ARRAY'],
			'3D.': ['-DTEX_TYPE_3D'],
		},
		'vertices.vert': {'': [] },
		'vertices.frag': {'': [] },
	}

	foreach file, configs : overlay_shaders
		foreach name, config : configs
			varname = file.underscorify() + '_' + name.underscorify() + 'spv_data'
			outname = file + '.' + name + 'spv.h'
			spv += custom_target(
				outname,
				input: files(file),
				output: outname,
				command : [glslang, '-V', '--vn', varname, config, '-o', '@OUTPUT@', '@INPUT@'])
		endforeach
	endforeach

	shader_inc = include_directories('.')
else
	warning('Did not find glslangValidator, will use prebuilt spirv shaders')
	shader_inc = include_directories('prebuilt')
endif

