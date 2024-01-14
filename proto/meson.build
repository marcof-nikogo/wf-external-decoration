wayland_scanner = find_program('wayland-scanner')

wayland_protos = dependency('wayland-protocols')
wl_protocol_dir = wayland_protos.get_pkgconfig_variable('pkgdatadir')

wayland_scanner_code = generator(
	wayland_scanner,
	output: '@BASENAME@-protocol.c',
	arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

wayland_scanner_client = generator(
	wayland_scanner,
	output: '@BASENAME@-client-protocol.h',
	arguments: ['client-header', '@INPUT@', '@OUTPUT@'],
)

wayland_scanner_server = generator(
	wayland_scanner,
	output: '@BASENAME@-protocol.h',
	arguments: ['server-header', '@INPUT@', '@OUTPUT@'],
)

client_protocols = [
    'wf-decorator.xml'
]

server_protocols = [
    [wl_protocol_dir, 'stable/xdg-shell/xdg-shell.xml'],
    'wf-decorator.xml'
]

# ------------- Client-side protocol
wf_client_protos_src = []
wf_client_protos_headers = []

foreach p : client_protocols
	xml = join_paths(p)
	wf_client_protos_headers += wayland_scanner_client.process(xml)
    wf_client_protos_src += wayland_scanner_code.process(xml)
endforeach

lib_wf_client_protos = static_library('wf_client_protos',
    wf_client_protos_src + wf_client_protos_headers,
    dependencies: [wayland_client])
wf_client_protos = declare_dependency(link_with: lib_wf_client_protos, sources: wf_client_protos_headers)

# ------------- Server-side protocol
wf_server_protos_src = []
wf_server_protos_headers = []

foreach p : server_protocols
	xml = join_paths(p)
	wf_server_protos_headers += wayland_scanner_server.process(xml)
    wf_server_protos_src += wayland_scanner_code.process(xml)
endforeach

lib_wf_server_protos = static_library('wf_server_protos',
    wf_server_protos_src + wf_server_protos_headers,
    dependencies: [wayland_server])
wf_server_protos = declare_dependency(link_with: lib_wf_server_protos, sources: wf_server_protos_headers)
