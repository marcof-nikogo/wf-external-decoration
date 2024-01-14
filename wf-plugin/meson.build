glib = dependency('glib-2.0')
plugin = shared_module(
    'wf-external-decorator',
    'wf-external-decor.cpp',
    dependencies: [wayfire, wlroots, wf_server_protos, glib],
    install: true,
    install_dir: wayfire.get_variable(pkgconfig: 'plugindir'))
