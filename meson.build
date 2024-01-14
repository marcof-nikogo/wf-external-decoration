project(
	'wf-external-decorator',
    'c',
	'cpp',
	version: '0.0',
	license: 'MIT',
	meson_version: '>=0.51.0',
	default_options: [
		'cpp_std=c++17',
        'c_std=c99',
		'warning_level=2',
		'werror=false',
	],
)
add_global_arguments(['-Wno-unused','-Wno-unused-parameter'],language: 'c')
add_global_arguments(['-Wno-unused','-Wno-unused-parameter'],language: 'cpp')

gtk3 = dependency('gtk+-3.0')
gdk_pixbuf = dependency('gdk-pixbuf-2.0')
wayfire = dependency('wayfire')
wlroots = dependency('wlroots')
wayland_client = dependency('wayland-client')
wayland_server = dependency('wayland-server')

project_args = ['-DWLR_USE_UNSTABLE']
add_project_arguments(project_args, language: ['cpp', 'c'])

subdir('proto')
subdir('wf-metacity-decorator')
subdir('wf-plugin')
subdir('metadata')
subdir('assets')
