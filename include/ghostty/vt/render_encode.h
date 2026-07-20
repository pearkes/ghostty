/**
 * @file render_encode.h
 *
 * Discussion draft for bulk, renderer-neutral Ghostty RenderState encoding.
 *
 * This is an experimental API proposal and has no implementation. If accepted,
 * these declarations would normally be integrated into <ghostty/vt/render.h>.
 * They remain separate on this branch so the contract and wire format can be
 * reviewed and compiled independently.
 */

#ifndef GHOSTTY_VT_RENDER_ENCODE_H
#define GHOSTTY_VT_RENDER_ENCODE_H

#include <stddef.h>
#include <stdint.h>

#include <ghostty/vt/render.h>
#include <ghostty/vt/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup render_encode Encoded Render State
 *
 * A bulk value projection of GhosttyRenderState for high-cost FFI boundaries.
 * The output contains terminal cells and authoritative RenderState damage. It
 * does not contain glyphs, shaping, renderer commands, browser state, or GPU
 * resources.
 *
 * Typical steady-state use is two calls per terminal update:
 *
 * @code
 * ghostty_render_state_update(state, terminal);
 * ghostty_render_state_encode(state, &options, &buffer);
 * @endcode
 *
 * The two-phase begin/end update API may be used instead when the caller needs
 * to minimize terminal lock duration. Encoding reads only the completed render
 * state and does not access the terminal.
 *
 * The caller owns and reuses the output allocation. A WebAssembly host reads
 * the output directly from linear memory after reacquiring its memory view.
 *
 * @{
 */

/** Versioned encoded representation selected by the caller. */
typedef enum GHOSTTY_ENUM_TYPED {
  /** Fixed-width row/cell records plus trailing grapheme UTF-8. */
  GHOSTTY_RENDER_STATE_ENCODE_FORMAT_CELLS_V1 = 1,
  GHOSTTY_RENDER_STATE_ENCODE_FORMAT_MAX_VALUE = GHOSTTY_ENUM_MAX_VALUE,
} GhosttyRenderStateEncodeFormat;

/** Options that change which RenderState damage is encoded or acknowledged. */
typedef enum GHOSTTY_ENUM_TYPED {
  GHOSTTY_RENDER_STATE_ENCODE_FLAG_NONE = 0,

  /** Emit every viewport row regardless of the current dirty state. */
  GHOSTTY_RENDER_STATE_ENCODE_FORCE_FULL = 1 << 0,

  /**
   * Atomically clear damage represented by a successful encode.
   *
   * No damage is cleared on GHOSTTY_OUT_OF_SPACE or any other error. A forced
   * full snapshot does not create damage; it only acknowledges damage that was
   * already present.
   */
  GHOSTTY_RENDER_STATE_ENCODE_CONSUME_DIRTY = 1 << 1,

  GHOSTTY_RENDER_STATE_ENCODE_FLAG_MAX_VALUE = GHOSTTY_ENUM_MAX_VALUE,
} GhosttyRenderStateEncodeFlag;

/**
 * Sized input options for ghostty_render_state_encode().
 *
 * Input compatibility follows Ghostty's sized-struct convention. Callers must
 * provide fields through format. Missing trailing fields are treated as zero,
 * and fields beyond the library's known size are ignored.
 */
typedef struct {
  /** Must be initialized with GHOSTTY_INIT_SIZED(). */
  size_t size;

  /** Encoded representation to produce. */
  GhosttyRenderStateEncodeFormat format;

  /** Bitwise combination of GhosttyRenderStateEncodeFlag values. */
  uint32_t flags;
} GhosttyRenderStateEncodeOptions;

/**
 * Encode a completed RenderState snapshot or its current damage.
 *
 * Damage mapping for Cells V1:
 *
 * - clean: emit no row records;
 * - partial: emit complete cells for dirty rows only;
 * - full: emit every viewport row;
 * - FORCE_FULL: emit every viewport row without manufacturing damage.
 *
 * Cursor metadata is present in the header even when no rows are emitted.
 * Palette changes follow Ghostty's existing full-damage behavior; every full
 * or forced-full Cells V1 snapshot includes the active 256-color palette.
 * A new consumer must begin with a full snapshot (normally the RenderState's
 * initial state) or request FORCE_FULL before accepting partial snapshots.
 *
 * output->len receives bytes written on GHOSTTY_SUCCESS. When output->ptr is
 * NULL, output->cap is zero, or capacity is insufficient, the function returns
 * GHOSTTY_OUT_OF_SPACE and output->len receives the exact required capacity.
 * Partially written bytes after an error are unspecified and must be ignored.
 * Cells V1 performs no allocation; steady-state encoding writes directly into
 * the reusable caller buffer.
 *
 * A successful encode is a value snapshot. Its bytes remain valid until the
 * caller overwrites or frees output->ptr. The function does not return borrowed
 * pointers into Ghostty row, grapheme, or style storage.
 *
 * With CONSUME_DIRTY, a successful partial encode clears every emitted row's
 * dirty flag and the global dirty state; a successful full encode clears all
 * row dirty flags and the global dirty state. Without CONSUME_DIRTY, encoding
 * is read-only. Errors clear neither layer.
 *
 * The caller must serialize access to state. Encoding may run without terminal
 * access after ghostty_render_state_update() or the corresponding end-update
 * phase has completed.
 *
 * @param state Completed render state to encode.
 * @param options Sized options selecting format and flags.
 * @param output Caller-owned output buffer.
 * @return GHOSTTY_SUCCESS, GHOSTTY_OUT_OF_SPACE, or GHOSTTY_INVALID_VALUE.
 *         Errors acknowledge no damage.
 */
GHOSTTY_API GhosttyResult ghostty_render_state_encode(
    GhosttyRenderState state,
    const GhosttyRenderStateEncodeOptions* options,
    GhosttyBuffer* output);

/* ------------------------------------------------------------------------- */
/* Cells V1 wire contract                                                    */
/* ------------------------------------------------------------------------- */

/** Little-endian u32 whose bytes are "GTRF". */
#define GHOSTTY_RENDER_STATE_CELLS_V1_MAGIC UINT32_C(0x46525447)
#define GHOSTTY_RENDER_STATE_CELLS_V1_VERSION UINT16_C(1)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_SIZE UINT16_C(80)
#define GHOSTTY_RENDER_STATE_CELLS_V1_ROW_SIZE UINT16_C(16)
#define GHOSTTY_RENDER_STATE_CELLS_V1_CELL_SIZE UINT16_C(32)

/**
 * Cells V1 is a byte format, not a C struct ABI.
 *
 * All integers are canonical little-endian. All offsets are uint32_t values
 * relative to the first encoded byte. Sections are four-byte aligned. A
 * consumer must use byte reads, memcpy, or an equivalent DataView rather than
 * casting the output to compiler-layout structs.
 *
 * Header layout:
 * @verbatim
 * +00 u32 magic             +04 u16 version
 * +06 u16 header_size       +08 u32 flags
 * +12 u32 snapshot_id       +16 u16 columns
 * +18 u16 rows              +20 u8  dirty
 * +21 u8  cursor_flags      +22 u8  cursor_style
 * +23 u8  reserved          +24 u16 cursor_x
 * +26 u16 cursor_y          +28 u16 emitted_rows
 * +30 u16 row_size          +32 u16 cell_size
 * +34 u16 reserved          +36 u32 row_offset
 * +40 u32 cell_offset       +44 u32 utf8_offset
 * +48 u32 byte_length       +52 u32 default_fg
 * +56 u32 default_bg        +60 u32 cursor_color
 * +64 u32 palette_offset    +68 u16 palette_count
 * +70 u16 palette_entry_size
 * +72 u32 cell_count        +76 u32 utf8_length
 * @endverbatim
 */

/* 80-byte header field offsets. */
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_MAGIC UINT32_C(0)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_VERSION UINT32_C(4)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_SIZE_FIELD UINT32_C(6)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_FLAGS UINT32_C(8)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_SNAPSHOT_ID UINT32_C(12)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_COLUMNS UINT32_C(16)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_ROWS UINT32_C(18)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_DIRTY UINT32_C(20)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_CURSOR_FLAGS UINT32_C(21)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_CURSOR_STYLE UINT32_C(22)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_CURSOR_X UINT32_C(24)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_CURSOR_Y UINT32_C(26)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_EMITTED_ROWS UINT32_C(28)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_ROW_SIZE UINT32_C(30)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_CELL_SIZE UINT32_C(32)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_ROW_OFFSET UINT32_C(36)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_CELL_OFFSET UINT32_C(40)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_UTF8_OFFSET UINT32_C(44)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_BYTE_LENGTH UINT32_C(48)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_DEFAULT_FG UINT32_C(52)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_DEFAULT_BG UINT32_C(56)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_CURSOR_COLOR UINT32_C(60)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_PALETTE_OFFSET UINT32_C(64)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_PALETTE_COUNT UINT32_C(68)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_PALETTE_ENTRY_SIZE UINT32_C(70)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_CELL_COUNT UINT32_C(72)
#define GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_UTF8_LENGTH UINT32_C(76)

/** Flags written to GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_FLAGS. */
typedef enum GHOSTTY_ENUM_TYPED {
  GHOSTTY_RENDER_STATE_CELLS_V1_OUTPUT_FLAG_NONE = 0,

  /** Every viewport row is present in this output. */
  GHOSTTY_RENDER_STATE_CELLS_V1_FULL_SNAPSHOT = 1 << 0,

  /** The full snapshot was requested with FORCE_FULL. */
  GHOSTTY_RENDER_STATE_CELLS_V1_FORCED_SNAPSHOT = 1 << 1,

  /** A palette section is present. V1 sets this on every full snapshot. */
  GHOSTTY_RENDER_STATE_CELLS_V1_PALETTE_PRESENT = 1 << 2,

  GHOSTTY_RENDER_STATE_CELLS_V1_OUTPUT_FLAG_MAX_VALUE = GHOSTTY_ENUM_MAX_VALUE,
} GhosttyRenderStateCellsV1OutputFlag;

/** Flags written to GHOSTTY_RENDER_STATE_CELLS_V1_HEADER_CURSOR_FLAGS. */
typedef enum GHOSTTY_ENUM_TYPED {
  GHOSTTY_RENDER_STATE_CELLS_V1_CURSOR_FLAG_NONE = 0,

  /** Cursor viewport coordinates are valid. */
  GHOSTTY_RENDER_STATE_CELLS_V1_CURSOR_HAS_POSITION = 1 << 0,

  /** Terminal modes currently permit cursor display. */
  GHOSTTY_RENDER_STATE_CELLS_V1_CURSOR_VISIBLE = 1 << 1,

  /** Cursor is configured to blink. */
  GHOSTTY_RENDER_STATE_CELLS_V1_CURSOR_BLINKING = 1 << 2,

  /** Cursor viewport position is the tail of a wide cell. */
  GHOSTTY_RENDER_STATE_CELLS_V1_CURSOR_WIDE_TAIL = 1 << 3,

  /** Cursor is at a password input field. */
  GHOSTTY_RENDER_STATE_CELLS_V1_CURSOR_PASSWORD_INPUT = 1 << 4,

  /** HEADER_CURSOR_COLOR contains an explicit RGB color. */
  GHOSTTY_RENDER_STATE_CELLS_V1_CURSOR_COLOR_PRESENT = 1 << 5,

  GHOSTTY_RENDER_STATE_CELLS_V1_CURSOR_FLAG_MAX_VALUE = GHOSTTY_ENUM_MAX_VALUE,
} GhosttyRenderStateCellsV1CursorFlag;

/**
 * Header field semantics.
 *
 * SNAPSHOT_ID is a wrapping identifier for the completed RenderState update;
 * retries and repeated encodes of the same completed update use the same id.
 * DIRTY is the pre-acknowledgement GhosttyRenderStateDirty value. Cursor style
 * uses GhosttyRenderStateCursorVisualStyle values. Cursor x/y are ignored
 * unless CURSOR_HAS_POSITION is set.
 *
 * Default foreground, default background, optional cursor color, and palette
 * entries store RGB in the low 24 bits as 0x00RRGGBB. Palette offset is zero
 * when no palette is emitted; full snapshots contain 256 four-byte entries.
 */

/* 16-byte row record field offsets. */
#define GHOSTTY_RENDER_STATE_CELLS_V1_ROW_Y UINT32_C(0)
#define GHOSTTY_RENDER_STATE_CELLS_V1_ROW_FLAGS UINT32_C(2)
#define GHOSTTY_RENDER_STATE_CELLS_V1_ROW_SELECTION_START UINT32_C(4)
#define GHOSTTY_RENDER_STATE_CELLS_V1_ROW_SELECTION_END UINT32_C(6)
#define GHOSTTY_RENDER_STATE_CELLS_V1_ROW_FIRST_CELL UINT32_C(8)
#define GHOSTTY_RENDER_STATE_CELLS_V1_ROW_CELL_COUNT UINT32_C(12)
#define GHOSTTY_RENDER_STATE_CELLS_V1_ROW_SEMANTIC_PROMPT UINT32_C(14)

#define GHOSTTY_RENDER_STATE_CELLS_V1_NO_SELECTION UINT16_C(0xffff)

/**
 * Row layout:
 * @verbatim
 * +00 u16 y                 +02 u16 flags
 * +04 u16 selection_start   +06 u16 selection_end
 * +08 u32 first_cell        +12 u16 cell_count
 * +14 u8  semantic_prompt   +15 u8  reserved
 * @endverbatim
 */

/** Flags written to GHOSTTY_RENDER_STATE_CELLS_V1_ROW_FLAGS. */
typedef enum GHOSTTY_ENUM_TYPED {
  GHOSTTY_RENDER_STATE_CELLS_V1_ROW_FLAG_NONE = 0,

  GHOSTTY_RENDER_STATE_CELLS_V1_ROW_WRAPPED = 1 << 0,
  GHOSTTY_RENDER_STATE_CELLS_V1_ROW_WRAP_CONTINUATION = 1 << 1,
  GHOSTTY_RENDER_STATE_CELLS_V1_ROW_HAS_GRAPHEMES = 1 << 2,
  GHOSTTY_RENDER_STATE_CELLS_V1_ROW_HAS_STYLING = 1 << 3,
  GHOSTTY_RENDER_STATE_CELLS_V1_ROW_HAS_HYPERLINKS = 1 << 4,
  GHOSTTY_RENDER_STATE_CELLS_V1_ROW_HAS_KITTY_PLACEHOLDER = 1 << 5,
  GHOSTTY_RENDER_STATE_CELLS_V1_ROW_FLAG_MAX_VALUE = GHOSTTY_ENUM_MAX_VALUE,
} GhosttyRenderStateCellsV1RowFlag;

/**
 * Row field semantics.
 *
 * Emitted rows are ordered by increasing viewport y. V1 emits exactly one
 * record per emitted row and exactly viewport-column-count cells for it.
 * Selection endpoints are inclusive and match
 * GhosttyRenderStateRowSelection. Both endpoints are NO_SELECTION when the row
 * does not intersect selection. SEMANTIC_PROMPT uses
 * GhosttyRowSemanticPrompt values.
 */

/* 32-byte cell record field offsets. */
#define GHOSTTY_RENDER_STATE_CELLS_V1_CELL_UTF8_RELATIVE_OFFSET UINT32_C(0)
#define GHOSTTY_RENDER_STATE_CELLS_V1_CELL_UTF8_LENGTH UINT32_C(4)
#define GHOSTTY_RENDER_STATE_CELLS_V1_CELL_WIDE UINT32_C(8)
#define GHOSTTY_RENDER_STATE_CELLS_V1_CELL_CONTENT_FLAGS UINT32_C(9)
#define GHOSTTY_RENDER_STATE_CELLS_V1_CELL_STYLE_FLAGS UINT32_C(10)
#define GHOSTTY_RENDER_STATE_CELLS_V1_CELL_SEMANTIC_CONTENT UINT32_C(12)
#define GHOSTTY_RENDER_STATE_CELLS_V1_CELL_FOREGROUND UINT32_C(16)
#define GHOSTTY_RENDER_STATE_CELLS_V1_CELL_BACKGROUND UINT32_C(20)
#define GHOSTTY_RENDER_STATE_CELLS_V1_CELL_UNDERLINE_COLOR UINT32_C(24)

/**
 * Cell layout:
 * @verbatim
 * +00 u32 utf8_relative_offset  +04 u32 utf8_length
 * +08 u8  wide                  +09 u8  content_flags
 * +10 u16 style_flags           +12 u8  semantic_content
 * +13 u8[3] reserved            +16 u32 foreground
 * +20 u32 background            +24 u32 underline_color
 * +28 u8[4] reserved
 * @endverbatim
 */

/** Flags written to GHOSTTY_RENDER_STATE_CELLS_V1_CELL_CONTENT_FLAGS. */
typedef enum GHOSTTY_ENUM_TYPED {
  GHOSTTY_RENDER_STATE_CELLS_V1_CELL_CONTENT_FLAG_NONE = 0,

  GHOSTTY_RENDER_STATE_CELLS_V1_CELL_HAS_TEXT = 1 << 0,
  GHOSTTY_RENDER_STATE_CELLS_V1_CELL_HAS_STYLING = 1 << 1,
  GHOSTTY_RENDER_STATE_CELLS_V1_CELL_HAS_HYPERLINK = 1 << 2,
  GHOSTTY_RENDER_STATE_CELLS_V1_CELL_PROTECTED = 1 << 3,
  GHOSTTY_RENDER_STATE_CELLS_V1_CELL_CONTENT_FLAG_MAX_VALUE = GHOSTTY_ENUM_MAX_VALUE,
} GhosttyRenderStateCellsV1CellContentFlag;

/** Style bits written to GHOSTTY_RENDER_STATE_CELLS_V1_CELL_STYLE_FLAGS. */
typedef enum GHOSTTY_ENUM_TYPED {
  GHOSTTY_RENDER_STATE_CELLS_V1_STYLE_FLAG_NONE = 0,

  GHOSTTY_RENDER_STATE_CELLS_V1_STYLE_BOLD = 1 << 0,
  GHOSTTY_RENDER_STATE_CELLS_V1_STYLE_ITALIC = 1 << 1,
  GHOSTTY_RENDER_STATE_CELLS_V1_STYLE_FAINT = 1 << 2,
  GHOSTTY_RENDER_STATE_CELLS_V1_STYLE_BLINK = 1 << 3,
  GHOSTTY_RENDER_STATE_CELLS_V1_STYLE_INVERSE = 1 << 4,
  GHOSTTY_RENDER_STATE_CELLS_V1_STYLE_INVISIBLE = 1 << 5,
  GHOSTTY_RENDER_STATE_CELLS_V1_STYLE_STRIKETHROUGH = 1 << 6,
  GHOSTTY_RENDER_STATE_CELLS_V1_STYLE_OVERLINE = 1 << 7,
  GHOSTTY_RENDER_STATE_CELLS_V1_STYLE_FLAG_MAX_VALUE = GHOSTTY_ENUM_MAX_VALUE,
} GhosttyRenderStateCellsV1StyleFlag;

#define GHOSTTY_RENDER_STATE_CELLS_V1_STYLE_UNDERLINE_SHIFT UINT32_C(8)
#define GHOSTTY_RENDER_STATE_CELLS_V1_STYLE_UNDERLINE_MASK UINT32_C(0x0700)

/**
 * Cell field semantics.
 *
 * UTF8_RELATIVE_OFFSET is relative to HEADER_UTF8_OFFSET. UTF8_LENGTH is u32,
 * because the existing public grapheme UTF-8 API reports size_t and does not
 * impose a u16 byte limit. The bytes contain the complete authoritative
 * grapheme cluster. Empty and spacer cells normally have length zero.
 *
 * WIDE uses public GhosttyCellWide values. SEMANTIC_CONTENT uses public
 * GhosttyCellSemanticContent values. Underline style is a
 * GhosttySgrUnderline value stored in STYLE_FLAGS bits 10..8.
 *
 * Hyperlink presence is hot metadata; hyperlink URI lookup remains a separate
 * cold API. Selection is row metadata and is not duplicated per cell.
 */

/** Wire-color tag stored in bits 31..30 of each cell color field. */
typedef enum GHOSTTY_ENUM_TYPED {
  /** Resolve through the frame header's default foreground/background. */
  GHOSTTY_RENDER_STATE_CELLS_V1_COLOR_DEFAULT = 0,

  /** Low eight bits contain the active palette index. */
  GHOSTTY_RENDER_STATE_CELLS_V1_COLOR_PALETTE = 1,

  /** Low 24 bits contain 0xRRGGBB. */
  GHOSTTY_RENDER_STATE_CELLS_V1_COLOR_RGB = 2,

  /** Reserved; consumers must reject this value in V1. */
  GHOSTTY_RENDER_STATE_CELLS_V1_COLOR_RESERVED = 3,

  GHOSTTY_RENDER_STATE_CELLS_V1_COLOR_KIND_MAX_VALUE = GHOSTTY_ENUM_MAX_VALUE,
} GhosttyRenderStateCellsV1ColorKind;

#define GHOSTTY_RENDER_STATE_CELLS_V1_COLOR_KIND_SHIFT UINT32_C(30)
#define GHOSTTY_RENDER_STATE_CELLS_V1_COLOR_KIND_MASK UINT32_C(0xc0000000)
#define GHOSTTY_RENDER_STATE_CELLS_V1_COLOR_PALETTE_MASK UINT32_C(0x000000ff)
#define GHOSTTY_RENDER_STATE_CELLS_V1_COLOR_RGB_MASK UINT32_C(0x00ffffff)

/**
 * Bounds and validation.
 *
 * Header byte length and all section offsets are u32. An encoder must reject a
 * Cells V1 result that cannot be represented within UINT32_MAX bytes and must
 * acknowledge no damage. Reserved bytes and bits are written as zero.
 * Consumers validate magic, version, record sizes, total length, aligned
 * non-overlapping sections, row/cell counts, viewport bounds, text ranges, and
 * supported color tags before using any record.
 */

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* GHOSTTY_VT_RENDER_ENCODE_H */
