/*
 * wui's FreeType module list, installed over the default
 * <freetype/config/ftmodule.h> by tools/prepare_dependencies.py.
 *
 * wui_text only accepts scalable SFNT faces (src/text/font.cpp) and renders
 * with FT_RENDER_MODE_NORMAL without FT_LOAD_COLOR, so the non-SFNT drivers
 * (Type 1, CID, PFR, Type 42, Windows FNT, PCF, BDF) and the mono, SDF and
 * OT-SVG renderers can never contribute to accepted text. Kept: TrueType and
 * CFF outlines, their PostScript helpers and hinters, and the autofitter.
 */

FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )

/* EOF */
