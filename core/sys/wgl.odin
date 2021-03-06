foreign_system_library "opengl32.lib" when ODIN_OS == "windows";
import . "windows.odin";

const (
	CONTEXT_MAJOR_VERSION_ARB          = 0x2091;
	CONTEXT_MINOR_VERSION_ARB          = 0x2092;
	CONTEXT_FLAGS_ARB                  = 0x2094;
	CONTEXT_PROFILE_MASK_ARB           = 0x9126;
	CONTEXT_FORWARD_COMPATIBLE_BIT_ARB = 0x0002;
	CONTEXT_CORE_PROFILE_BIT_ARB       = 0x00000001;
	CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB = 0x00000002;
)

type (
	Hglrc    Handle;
	ColorRef u32;

	LayerPlaneDescriptor struct {
		size:             u16,
		version:          u16,
		flags:            u32,
		pixel_type:       u8,
		color_bits:       u8,
		red_bits:         u8,
		red_shift:        u8,
		green_bits:       u8,
		green_shift:      u8,
		blue_bits:        u8,
		blue_shift:       u8,
		alpha_bits:       u8,
		alpha_shift:      u8,
		accum_bits:       u8,
		accum_red_bits:   u8,
		accum_green_bits: u8,
		accum_blue_bits:  u8,
		accum_alpha_bits: u8,
		depth_bits:       u8,
		stencil_bits:     u8,
		aux_buffers:      u8,
		layer_type:       u8,
		reserved:         u8,
		transparent:      ColorRef,
	}

	PointFloat struct {
		x, y: f32,
	}

	Glyph_MetricsFloat struct {
		black_box_x:  f32,
		black_box_y:  f32,
		glyph_origin: PointFloat,
		cell_inc_x:   f32,
		cell_inc_y:   f32,
	}
)

type (
	CreateContextAttribsARBType proc(hdc: Hdc, h_share_context: rawptr, attribList: ^i32) -> Hglrc;
	ChoosePixelFormatARBType    proc(hdc: Hdc, attrib_i_list: ^i32, attrib_f_list: ^f32, max_formats: u32, formats: ^i32, num_formats : ^u32) -> Bool #cc_c;
	SwapIntervalEXTType         proc(interval: i32) -> bool #cc_c;
	GetExtensionsStringARBType  proc(Hdc) -> ^u8 #cc_c;
)

var (
	create_context_attribs_arb: CreateContextAttribsARBType;
	choose_pixel_format_arb:    ChoosePixelFormatARBType;
	swap_interval_ext:          SwapIntervalEXTType;
	get_extensions_string_arb:  GetExtensionsStringARBType;
)


foreign opengl32 {
	proc create_context           (hdc: Hdc) -> Hglrc                                                                                                 #link_name "wglCreateContext";
	proc make_current             (hdc: Hdc, hglrc: Hglrc) -> Bool                                                                                    #link_name "wglMakeCurrent";
	proc get_proc_address         (c_str: ^u8) -> Proc                                                                                                #link_name "wglGetProcAddress";
	proc delete_context           (hglrc: Hglrc) -> Bool                                                                                              #link_name "wglDeleteContext";
	proc copy_context             (src, dst: Hglrc, mask: u32) -> Bool                                                                                #link_name "wglCopyContext";
	proc create_layer_context     (hdc: Hdc, layer_plane: i32) -> Hglrc                                                                               #link_name "wglCreateLayerContext";
	proc describe_layer_plane     (hdc: Hdc, pixel_format, layer_plane: i32, bytes: u32, pd: ^LayerPlaneDescriptor) -> Bool                           #link_name "wglDescribeLayerPlane";
	proc get_current_context      () -> Hglrc                                                                                                         #link_name "wglGetCurrentContext";
	proc get_current_dc           () -> Hdc                                                                                                           #link_name "wglGetCurrentDC";
	proc get_layer_palette_entries(hdc: Hdc, layer_plane, start, entries: i32, cr: ^ColorRef) -> i32                                                  #link_name "wglGetLayerPaletteEntries";
	proc realize_layer_palette    (hdc: Hdc, layer_plane: i32, realize: Bool) -> Bool                                                                 #link_name "wglRealizeLayerPalette";
	proc set_layer_palette_entries(hdc: Hdc, layer_plane, start, entries: i32, cr: ^ColorRef) -> i32                                                  #link_name "wglSetLayerPaletteEntries";
	proc share_lists              (hglrc1, hglrc2: Hglrc) -> Bool                                                                                     #link_name "wglShareLists";
	proc swap_layer_buffers       (hdc: Hdc, planes: u32) -> Bool                                                                                     #link_name "wglSwapLayerBuffers";
	proc use_font_bitmaps         (hdc: Hdc, first, count, list_base: u32) -> Bool                                                                    #link_name "wglUseFontBitmaps";
	proc use_font_outlines        (hdc: Hdc, first, count, list_base: u32, deviation, extrusion: f32, format: i32, gmf: ^Glyph_MetricsFloat) -> Bool  #link_name "wglUseFontOutlines";
}
