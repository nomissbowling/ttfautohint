/* taprep.c */

/* written 2011 by Werner Lemberg <wl@gnu.org> */

#include "tabytecode.h"


#define PREP(snippet_name) prep_ ## snippet_name

/* we often need 0x10000 which can't be pushed directly onto the stack, */
/* thus we provide it in the storage area */

unsigned char PREP(store_0x10000) [] = {

  PUSHB_1,
    sal_0x10000,
  PUSHW_2,
    0x08, /* 0x800 */
    0x00,
    0x08, /* 0x800 */
    0x00,
  MUL, /* 0x10000 */
  WS,

};

unsigned char PREP(align_top_a) [] = {

  /* optimize the alignment of the top of small letters to the pixel grid */

  PUSHB_1,

};

/*  %c, index of alignment blue zone */

unsigned char PREP(align_top_b) [] = {

  RCVT,
  DUP,
  DUP,
  PUSHB_1,
    40,
  ADD,
  FLOOR, /* fitted = FLOOR(scaled + 40) */
  DUP, /* s: scaled scaled fitted fitted */
  ROLL,
  NEQ,
  IF, /* s: scaled fitted */
    PUSHB_1,
      sal_0x10000,
    RS,
    MUL, /* scaled in 16.16 format */
    SWAP,
    DIV, /* (fitted / scaled) in 16.16 format */

    PUSHB_1,
      sal_scale,
    SWAP,
    WS,

};

unsigned char PREP(loop_cvt_a) [] = {

    /* loop over vertical CVT entries */
    PUSHB_4,

};

/*    %c, first vertical index */
/*    %c, last vertical index */

unsigned char PREP(loop_cvt_b) [] = {

      bci_cvt_rescale,
      bci_loop,
    CALL,

    /* loop over blue refs */
    PUSHB_4,

};

/*    %c, first blue ref index */
/*    %c, last blue ref index */

unsigned char PREP(loop_cvt_c) [] = {

      bci_cvt_rescale,
      bci_loop,
    CALL,

    /* loop over blue shoots */
    PUSHB_4,

};

/*    %c, first blue shoot index */
/*    %c, last blue shoot index */

unsigned char PREP(loop_cvt_d) [] = {

      bci_cvt_rescale,
      bci_loop,
    CALL,
  EIF,

};

unsigned char PREP(compute_extra_light_a) [] = {

  /* compute (vertical) `extra_light' flag */
  PUSHB_3,
    sal_is_extra_light,
    40,

};

/*  %c, index of vertical standard_width */

unsigned char PREP(compute_extra_light_b) [] = {

  RCVT,
  GT, /* standard_width < 40 */
  WS,

};

unsigned char PREP(round_blues_a) [] = {

  /* use discrete values for blue zone widths */
  PUSHB_4,

};

/*  %c, first blue ref index */
/*  %c, last blue ref index */

unsigned char PREP(round_blues_b) [] = {

    bci_blue_round,
    bci_loop,
  CALL

};


#define COPY_PREP(snippet_name) \
          memcpy(buf_p, prep_ ## snippet_name, \
                 sizeof (prep_ ## snippet_name)); \
          buf_p += sizeof (prep_ ## snippet_name);

static FT_Error
TA_table_build_prep(FT_Byte** prep,
                    FT_ULong* prep_len,
                    FONT* font)
{
  TA_LatinAxis vaxis;
  TA_LatinBlue blue_adjustment;
  FT_UInt i;

  FT_UInt buf_len;
  FT_UInt len;
  FT_Byte* buf;
  FT_Byte* buf_p;


  vaxis = &((TA_LatinMetrics)font->loader->hints.metrics)->axis[1];
  blue_adjustment = NULL;

  for (i = 0; i < vaxis->blue_count; i++)
  {
    if (vaxis->blues[i].flags & TA_LATIN_BLUE_ADJUSTMENT)
    {
      blue_adjustment = &vaxis->blues[i];
      break;
    }
  }

  buf_len = sizeof (PREP(store_0x10000));

  if (blue_adjustment)
    buf_len += sizeof (PREP(align_top_a))
               + 1
               + sizeof (PREP(align_top_b))
               + sizeof (PREP(loop_cvt_a))
               + 2
               + sizeof (PREP(loop_cvt_b))
               + 2
               + sizeof (PREP(loop_cvt_c))
               + 2
               + sizeof (PREP(loop_cvt_d));

  buf_len += sizeof (PREP(compute_extra_light_a))
             + 1
             + sizeof (PREP(compute_extra_light_b));

  if (CVT_BLUES_SIZE(font))
    buf_len += sizeof (PREP(round_blues_a))
               + 2
               + sizeof (PREP(round_blues_b));

  /* buffer length must be a multiple of four */
  len = (buf_len + 3) & ~3;
  buf = (FT_Byte*)malloc(len);
  if (!buf)
    return FT_Err_Out_Of_Memory;

  /* pad end of buffer with zeros */
  buf[len - 1] = 0x00;
  buf[len - 2] = 0x00;
  buf[len - 3] = 0x00;

  /* copy cvt program into buffer and fill in the missing variables */
  buf_p = buf;

  COPY_PREP(store_0x10000);

  if (blue_adjustment)
  {
    COPY_PREP(align_top_a);
    *(buf_p++) = (unsigned char)(CVT_BLUE_SHOOTS_OFFSET(font)
                                 + blue_adjustment - vaxis->blues);
    COPY_PREP(align_top_b);

    COPY_PREP(loop_cvt_a);
    *(buf_p++) = (unsigned char)CVT_VERT_WIDTHS_OFFSET(font);
    *(buf_p++) = (unsigned char)(CVT_VERT_WIDTHS_OFFSET(font)
                                 + CVT_VERT_WIDTHS_SIZE(font) - 1);
    COPY_PREP(loop_cvt_b);
    *(buf_p++) = (unsigned char)CVT_BLUE_REFS_OFFSET(font);
    *(buf_p++) = (unsigned char)(CVT_BLUE_REFS_OFFSET(font)
                                 + CVT_BLUES_SIZE(font) - 1);
    COPY_PREP(loop_cvt_c);
    *(buf_p++) = (unsigned char)CVT_BLUE_SHOOTS_OFFSET(font);
    *(buf_p++) = (unsigned char)(CVT_BLUE_SHOOTS_OFFSET(font)
                                 + CVT_BLUES_SIZE(font) - 1);
    COPY_PREP(loop_cvt_d);
  }

  COPY_PREP(compute_extra_light_a);
  *(buf_p++) = (unsigned char)CVT_VERT_STANDARD_WIDTH_OFFSET(font);
  COPY_PREP(compute_extra_light_b);

  if (CVT_BLUES_SIZE(font))
  {
    COPY_PREP(round_blues_a);
    *(buf_p++) = (unsigned char)CVT_BLUE_REFS_OFFSET(font);
    *(buf_p++) = (unsigned char)(CVT_BLUE_REFS_OFFSET(font)
                                 + CVT_BLUES_SIZE(font) - 1);
    COPY_PREP(round_blues_b);
  }

  *prep = buf;
  *prep_len = buf_len;

  return FT_Err_Ok;
}


FT_Error
TA_sfnt_build_prep_table(SFNT* sfnt,
                         FONT* font)
{
  FT_Error error;

  FT_Byte* prep_buf;
  FT_ULong prep_len;


  error = TA_sfnt_add_table_info(sfnt);
  if (error)
    return error;

  error = TA_table_build_prep(&prep_buf, &prep_len, font);
  if (error)
    return error;

  /* in case of success, `prep_buf' gets linked */
  /* and is eventually freed in `TA_font_unload' */
  error = TA_font_add_table(font,
                            &sfnt->table_infos[sfnt->num_table_infos - 1],
                            TTAG_prep, prep_len, prep_buf);
  if (error)
  {
    free(prep_buf);
    return error;
  }

  return FT_Err_Ok;
}

/* end of taprep.c */