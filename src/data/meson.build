# needed shaders
spv = []
glslang = find_program('glslangValidator', required: false)

if glslang.found()
	imgtable = {
		'1DArray.': ['-DTEX_TYPE_1D_ARRAY', '-DTEX_FORMAT_FLOAT'],
		'u1DArray.': ['-DTEX_TYPE_1D_ARRAY', '-DTEX_FORMAT_UINT'],
		'i1DArray.': ['-DTEX_TYPE_1D_ARRAY', '-DTEX_FORMAT_INT'],

		'2DArray.': ['-DTEX_TYPE_2D_ARRAY', '-DTEX_FORMAT_FLOAT'],
		'u2DArray.': ['-DTEX_TYPE_2D_ARRAY', '-DTEX_FORMAT_UINT'],
		'i2DArray.': ['-DTEX_TYPE_2D_ARRAY', '-DTEX_FORMAT_INT'],

		'3D.': ['-DTEX_TYPE_3D', '-DTEX_FORMAT_FLOAT'],
		'u3D.': ['-DTEX_TYPE_3D', '-DTEX_FORMAT_UINT'],
		'i3D.': ['-DTEX_TYPE_3D', '-DTEX_FORMAT_INT'],
	}

	overlay_shaders = {
		# the main imgui shaders used for text, shapes
		'gui.frag': { '': [] },
		'gui.vert': { '': [] },
		# image.frag for rendering images
		'image.frag': imgtable,
		# rendering transparent image background
		'imagebg.vert': {'': [] },
		'imagebg.frag': {'': [] },
		# readTex.comp for reading texture values
		'readTex.comp': imgtable,
		# vertex viewer
		'frustum.vert': {'': [] },
		'vertices.vert': {'': [] },
		'vertices.frag': {'': [] },
		# gui background blur
		'blur.comp': {'': [] },
		# accelStruct building
		'accelStructVertices.comp': {'': [] },
		# image analyze shaders
		'histogram.comp': imgtable,
		'minmax.comp': imgtable,
		# copyTex to sample-copy an image to a vec4[] buffer
		'copyTex.comp': imgtable,
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

