/* ttfautohint.c */

/* written 2011 by Werner Lemberg <wl@gnu.org> */

/* This file needs FreeType 2.4.5 or newer. */


#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

#include "ttfautohint.h"


/* these macros convert 16bit and 32bit numbers into single bytes */
/* using the byte order needed within SFNT files */

#define HIGH(x) (FT_Byte)(((x) & 0xFF00) >> 8)
#define LOW(x) ((x) & 0x00FF)

#define BYTE1(x) (FT_Byte)(((x) & 0xFF000000UL) >> 24);
#define BYTE2(x) (FT_Byte)(((x) & 0x00FF0000UL) >> 16);
#define BYTE3(x) (FT_Byte)(((x) & 0x0000FF00UL) >> 8);
#define BYTE4(x) ((x) & 0x000000FFUL);


/* the length of a dummy `DSIG' table */
#define DSIG_LEN 8

/* an empty slot in the table info array */
#define MISSING (FT_ULong)~0

/* the offset to the loca table format in `head' table */
#define LOCA_FORMAT_OFFSET 51

/* various offset within the `maxp' table */
#define MAXP_MAX_ZONES_OFFSET 14
#define MAXP_MAX_TWILIGHT_POINTS_OFFSET 16
#define MAXP_MAX_STORAGE_OFFSET 18
#define MAXP_MAX_FUNCTION_DEFS_OFFSET 20
#define MAXP_MAX_INSTRUCTION_DEFS_OFFSET 22
#define MAXP_MAX_STACK_ELEMENTS_OFFSET 24
#define MAXP_MAX_INSTRUCTIONS_OFFSET 26

#define MAXP_LEN 32


/* composite glyph flags */
#define ARGS_ARE_WORDS 0x0001
#define ARGS_ARE_XY_VALUES 0x0002
#define WE_HAVE_A_SCALE 0x0008
#define MORE_COMPONENTS 0x0020
#define WE_HAVE_AN_XY_SCALE 0x0040
#define WE_HAVE_A_2X2 0x0080
#define WE_HAVE_INSTR 0x0100


/* simple glyph flags */
#define X_SHORT_VECTOR 0x02
#define Y_SHORT_VECTOR 0x04
#define REPEAT 0x08
#define SAME_X 0x10
#define SAME_Y 0x20


/* a single glyph */
typedef struct GLYPH_
{
  FT_ULong len;
  FT_Byte* buf;
} GLYPH;

/* a representation of the data in the `glyf' table */
typedef struct glyf_Data_
{
  FT_UShort num_glyphs;
  GLYPH* glyphs;
} glyf_Data;

/* an SFNT table */
typedef struct SFNT_Table_ {
  FT_ULong tag;
  FT_ULong len;
  FT_Byte* buf; /* the table data */
  FT_ULong offset; /* from beginning of file */
  FT_ULong checksum;
  void* data; /* used e.g. for `glyf' table tada */
  FT_Bool recreated;
} SFNT_Table;

/* we use indices into the SFNT table array to */
/* represent table info records in a TTF header */
typedef FT_ULong SFNT_Table_Info;

/* this structure is used to model a TTF or a subfont within a TTC */
typedef struct SFNT_ {
  FT_Face face;

  SFNT_Table_Info* table_infos;
  FT_ULong num_table_infos;

  /* various SFNT table indices */
  FT_ULong glyf_idx;
  FT_ULong loca_idx;
  FT_ULong head_idx;
  FT_ULong maxp_idx;
} SFNT;

/* our font object */
typedef struct FONT_ {
  FT_Library lib;

  FT_Byte* in_buf;
  size_t in_len;

  FT_Byte* out_buf;
  size_t out_len;

  SFNT* sfnts;
  FT_Long num_sfnts;

  SFNT_Table* tables;
  FT_ULong num_tables;
} FONT;


static FT_Error
TA_font_read(FONT* font,
             FILE* in)
{
  fseek(in, 0, SEEK_END);
  font->in_len = ftell(in);
  fseek(in, 0, SEEK_SET);

  /* a valid TTF can never be that small */
  if (font->in_len < 100)
    return FT_Err_Invalid_Argument;

  font->in_buf = (FT_Byte*)malloc(font->in_len);
  if (!font->in_buf)
    return FT_Err_Out_Of_Memory;

  if (fread(font->in_buf, 1, font->in_len, in) != font->in_len)
    return FT_Err_Invalid_Stream_Read;

  return TA_Err_Ok;
}


static FT_Error
TA_font_init(FONT* font)
{
  FT_Error error;
  FT_Face f;


  error = FT_Init_FreeType(&font->lib);
  if (error)
    return error;

  /* get number of faces (i.e. subfonts) */
  error = FT_New_Memory_Face(font->lib, font->in_buf, font->in_len, -1, &f);
  if (error)
    return error;
  font->num_sfnts = f->num_faces;
  FT_Done_Face(f);

  /* it is a TTC if we have more than a single subfont */
  font->sfnts = (SFNT*)calloc(1, font->num_sfnts * sizeof (SFNT));
  if (!font->sfnts)
    return FT_Err_Out_Of_Memory;

  return TA_Err_Ok;
}


static FT_Error
TA_sfnt_add_table_info(SFNT* sfnt)
{
  SFNT_Table_Info* table_infos_new;


  sfnt->num_table_infos++;
  table_infos_new =
    (SFNT_Table_Info*)realloc(sfnt->table_infos, sfnt->num_table_infos
                                                 * sizeof (SFNT_Table_Info));
  if (!table_infos_new)
  {
    sfnt->num_table_infos--;
    return FT_Err_Out_Of_Memory;
  }
  else
    sfnt->table_infos = table_infos_new;

  sfnt->table_infos[sfnt->num_table_infos - 1] = MISSING;

  return TA_Err_Ok;
}


static FT_ULong
TA_table_compute_checksum(FT_Byte* buf,
                          FT_ULong len)
{
  FT_Byte* end_buf = buf + len;
  FT_ULong checksum = 0;


  /* we expect that `len' is a multiple of 4 */

  while (buf < end_buf)
  {
    checksum += *(buf++) << 24;
    checksum += *(buf++) << 16;
    checksum += *(buf++) << 8;
    checksum += *(buf++);
  }

  return checksum;
}


static FT_Error
TA_font_add_table(FONT* font,
                  SFNT_Table_Info* table_info,
                  FT_ULong tag,
                  FT_ULong len,
                  FT_Byte* buf)
{
  SFNT_Table* tables_new;
  SFNT_Table* table_last;


  font->num_tables++;
  tables_new = (SFNT_Table*)realloc(font->tables,
                                    font->num_tables * sizeof (SFNT_Table));
  if (!tables_new)
  {
    font->num_tables--;
    return FT_Err_Out_Of_Memory;
  }
  else
    font->tables = tables_new;

  table_last = &font->tables[font->num_tables - 1];

  table_last->tag = tag;
  table_last->len = len;
  table_last->buf = buf;
  table_last->checksum = TA_table_compute_checksum(buf, len);
  table_last->offset = 0; /* set in `TA_font_compute_table_offsets' */
  table_last->data = NULL;
  table_last->recreated = 0;

  /* link table and table info */
  *table_info = font->num_tables - 1;

  return TA_Err_Ok;
}


static void
TA_sfnt_sort_table_info(SFNT* sfnt,
                        FONT* font)
{
  /* Looking into an arbitrary TTF (with a `DSIG' table), tags */
  /* starting with an uppercase letter are sorted before lowercase */
  /* letters.  In other words, the alphabetical ordering (as */
  /* mandated by signing a font) is a simple numeric comparison of */
  /* the 32bit tag values. */

  SFNT_Table* tables = font->tables;

  SFNT_Table_Info* table_infos = sfnt->table_infos;
  FT_ULong num_table_infos = sfnt->num_table_infos;

  FT_ULong i;
  FT_ULong j;


  for (i = 1; i < num_table_infos; i++)
  {
    for (j = i; j > 0; j--)
    {
      SFNT_Table_Info swap;
      FT_ULong tag1;
      FT_ULong tag2;


      tag1 = (table_infos[j] == MISSING)
               ? 0
               : tables[table_infos[j]].tag;
      tag2 = (table_infos[j - 1] == MISSING)
               ? 0
               : tables[table_infos[j - 1]].tag;

      if (tag1 > tag2)
        break;

      swap = table_infos[j];
      table_infos[j] = table_infos[j - 1];
      table_infos[j - 1] = swap;
    }
  }
}


static FT_Error
TA_sfnt_split_into_SFNT_tables(SFNT* sfnt,
                               FONT* font)
{
  FT_Error error;
  FT_ULong i;


  /* basic check whether font is a TTF or TTC */
  if (!FT_IS_SFNT(sfnt->face))
    return FT_Err_Invalid_Argument;

  error = FT_Sfnt_Table_Info(sfnt->face, 0, NULL, &sfnt->num_table_infos);
  if (error)
    return error;

  sfnt->table_infos = (SFNT_Table_Info*)malloc(sfnt->num_table_infos
                                               * sizeof (SFNT_Table_Info));
  if (!sfnt->table_infos)
    return FT_Err_Out_Of_Memory;

  /* collect SFNT tables and search for `glyf' and `loca' table */
  sfnt->glyf_idx = MISSING;
  sfnt->loca_idx = MISSING;
  sfnt->head_idx = MISSING;
  sfnt->maxp_idx = MISSING;
  for (i = 0; i < sfnt->num_table_infos; i++)
  {
    SFNT_Table_Info* table_info = &sfnt->table_infos[i];
    FT_ULong tag;
    FT_ULong len;
    FT_Byte* buf;

    FT_ULong buf_len;
    FT_ULong j;


    *table_info = MISSING;

    error = FT_Sfnt_Table_Info(sfnt->face, i, &tag, &len);
    if (error)
    {
      /* this ignores both missing and zero-length tables */
      if (error == FT_Err_Table_Missing)
        continue;
      else
        return error;
    }

    /* ignore tables which we are going to create by ourselves */
    else if (tag == TTAG_fpgm
             || tag == TTAG_prep
             || tag == TTAG_cvt
             || tag == TTAG_DSIG)
      continue;

    /* make the allocated buffer length a multiple of 4 */
    buf_len = (len + 3) & -3;
    buf = (FT_Byte*)malloc(buf_len);
    if (!buf)
      return FT_Err_Out_Of_Memory;

    /* pad end of buffer with zeros */
    buf[buf_len - 1] = 0x00;
    buf[buf_len - 2] = 0x00;
    buf[buf_len - 3] = 0x00;

    /* load table */
    error = FT_Load_Sfnt_Table(sfnt->face, tag, 0, buf, &len);
    if (error)
      goto Err;

    /* check whether we already have this table */
    for (j = 0; j < font->num_tables; j++)
    {
      SFNT_Table* table = &font->tables[j];


      if (table->tag == tag
          && table->len == len
          && !memcmp(table->buf, buf, len))
        break;
    }

    if (tag == TTAG_head)
      sfnt->head_idx = j;
    else if (tag == TTAG_glyf)
      sfnt->glyf_idx = j;
    else if (tag == TTAG_loca)
      sfnt->loca_idx = j;
    else if (tag == TTAG_maxp)
      sfnt->maxp_idx = j;

    if (j == font->num_tables)
    {
      /* add element to table array if it is missing or different; */
      /* in case of success, `buf' gets linked and is eventually */
      /* freed in `TA_font_unload' */
      error = TA_font_add_table(font, table_info, tag, len, buf);
      if (error)
        goto Err;
    }
    else
    {
      /* reuse existing SFNT table */
      free(buf);
      *table_info = j;
    }
    continue;

  Err:
    free(buf);
    return error;
  }

  /* no (non-empty) `glyf', `loca', `head', or `maxp' table; */
  /* this can't be a valid TTF with outlines */
  if (sfnt->glyf_idx == MISSING
      || sfnt->loca_idx == MISSING
      || sfnt->head_idx == MISSING
      || sfnt->maxp_idx == MISSING)
    return FT_Err_Invalid_Argument;

  return TA_Err_Ok;
}


static FT_Error
TA_glyph_parse_composite(GLYPH* glyph,
                         FT_Byte* buf,
                         FT_ULong len)
{
  FT_ULong flags_offset; /* after the loop, this is the offset */
                         /* to last element in the flags array */
  FT_UShort flags;

  FT_Byte* p;
  FT_Byte* endp;


  p = buf;
  endp = buf + len;

  /* skip header */
  p += 10;

  /* walk over component records */
  do
  {
    if (p + 4 > endp)
      return FT_Err_Invalid_Table;

    flags_offset = p - buf;

    flags = *(p++) << 8;
    flags += *(p++);

    /* skip glyph component index */
    p += 2;

    /* skip scaling and offset arguments */
    if (flags & ARGS_ARE_WORDS)
      p += 4;
    else
      p += 2;

    if (flags & WE_HAVE_A_SCALE)
      p += 2;
    else if (flags & WE_HAVE_AN_XY_SCALE)
      p += 4;
    else if (flags & WE_HAVE_A_2X2)
      p += 8;
  } while (flags & MORE_COMPONENTS);

  /* adjust glyph record length */
  len = p - buf;

  glyph->len = len;
  glyph->buf = (FT_Byte*)malloc(len);
  if (!glyph->buf)
    return FT_Err_Out_Of_Memory;

  /* copy record without instructions (if any) */
  memcpy(glyph->buf, buf, len);
  glyph->buf[flags_offset] &= ~(WE_HAVE_INSTR >> 8);

  return TA_Err_Ok;
}


static FT_Error
TA_glyph_parse_simple(GLYPH* glyph,
                      FT_Byte* buf,
                      FT_UShort num_contours,
                      FT_ULong len)
{
  FT_ULong ins_offset;
  FT_Byte* flags_start;

  FT_UShort num_ins;
  FT_UShort num_pts;

  FT_ULong flags_size; /* size of the flags array */
  FT_ULong xy_size; /* size of x and y coordinate arrays together */

  FT_Byte* p;
  FT_Byte* endp;

  FT_UShort i;


  p = buf;
  endp = buf + len;

  ins_offset = 10 + num_contours * 2;

  p += ins_offset;

  if (p + 2 > endp)
    return FT_Err_Invalid_Table;

  /* get number of instructions */
  num_ins = *(p++) << 8;
  num_ins += *(p++);

  p += num_ins;

  if (p > endp)
    return FT_Err_Invalid_Table;

  /* get number of points from last outline point */
  num_pts = buf[ins_offset - 2] << 8;
  num_pts += buf[ins_offset - 1];
  num_pts++;

  flags_start = p;
  xy_size = 0;
  i = 0;

  while (i < num_pts)
  {
    FT_Byte flags;
    FT_Byte x_short;
    FT_Byte y_short;
    FT_Byte have_x;
    FT_Byte have_y;
    FT_Byte count;


    if (p + 1 > endp)
      return FT_Err_Invalid_Table;

    flags = *(p++);

    x_short = (flags & X_SHORT_VECTOR) ? 1 : 2;
    y_short = (flags & Y_SHORT_VECTOR) ? 1 : 2;

    have_x = ((flags & SAME_X) && !(flags & X_SHORT_VECTOR)) ? 0 : 1;
    have_y = ((flags & SAME_Y) && !(flags & Y_SHORT_VECTOR)) ? 0 : 1;

    count = 1;

    if (flags & REPEAT)
    {
      if (p + 1 > endp)
        return FT_Err_Invalid_Table;

      count += *(p++);

      if (i + count > num_pts)
        return FT_Err_Invalid_Table;
    }

    xy_size += count * x_short * have_x;
    xy_size += count * y_short * have_y;

    i += count;
  }

  if (p + xy_size > endp)
    return FT_Err_Invalid_Table;

  flags_size = p - flags_start;

  /* adjust glyph record length */
  glyph->len = ins_offset + 2 + flags_size + xy_size;
  glyph->buf = (FT_Byte*)malloc(glyph->len);
  if (!glyph->buf)
    return FT_Err_Out_Of_Memory;

  /* now copy everything but the instructions */
  memcpy(glyph->buf, buf, ins_offset);
  glyph->buf[ins_offset] = 0; /* set instructionLength field to zero */
  glyph->buf[ins_offset + 1] = 0;
  memcpy(glyph->buf + ins_offset + 2, flags_start, flags_size + xy_size);

  return TA_Err_Ok;
}


static FT_Error
TA_sfnt_split_glyf_table(SFNT* sfnt,
                         FONT* font)
{
  SFNT_Table* glyf_table = &font->tables[sfnt->glyf_idx];
  SFNT_Table* loca_table = &font->tables[sfnt->loca_idx];
  SFNT_Table* head_table = &font->tables[sfnt->head_idx];
  glyf_Data* data;
  FT_Byte loca_format;

  FT_ULong offset;
  FT_ULong offset_next;

  FT_Byte* p;
  FT_UShort i;


  /* in case of success, all allocated arrays are */
  /* linked and eventually freed in `TA_font_unload' */

  /* nothing to do if table has already been processed */
  if (glyf_table->data)
    return TA_Err_Ok;

  data = (glyf_Data*)calloc(1, sizeof (glyf_Data));
  if (!data)
    return FT_Err_Out_Of_Memory;

  glyf_table->data = data;

  loca_format = head_table->buf[LOCA_FORMAT_OFFSET];

  data->num_glyphs = loca_format ? loca_table->len / 4 - 1
                                 : loca_table->len / 2 - 1;
  data->glyphs = (GLYPH*)calloc(1, data->num_glyphs * sizeof (GLYPH));
  if (!data->glyphs)
    return FT_Err_Out_Of_Memory;

  p = loca_table->buf;

  if (loca_format)
  {
    offset_next = *(p++) << 24;
    offset_next += *(p++) << 16;
    offset_next += *(p++) << 8;
    offset_next += *(p++);
  }
  else
  {
    offset_next = *(p++) << 8;
    offset_next += *(p++);
    offset_next <<= 1;
  }

  /* loop over `loca' and `glyf' data */
  for (i = 0; i < data->num_glyphs; i++)
  {
    GLYPH* glyph = &data->glyphs[i];
    FT_ULong len;


    offset = offset_next;

    if (loca_format)
    {
      offset_next = *(p++) << 24;
      offset_next += *(p++) << 16;
      offset_next += *(p++) << 8;
      offset_next += *(p++);
    }
    else
    {
      offset_next = *(p++) << 8;
      offset_next += *(p++);
      offset_next <<= 1;
    }

    if (offset_next < offset
        || offset_next > glyf_table->len)
      return FT_Err_Invalid_Table;

    len = offset_next - offset;
    if (!len)
      continue; /* empty glyph */
    else
    {
      FT_Byte* buf;
      FT_Short num_contours;
      FT_Error error;


      /* check header size */
      if (len < 10)
        return FT_Err_Invalid_Table;

      buf = glyf_table->buf + offset;
      num_contours = (FT_Short)((buf[0] << 8) + buf[1]);

      /* We must parse the rest of the glyph record to get the exact */
      /* record length.  Since the `loca' table rounds record lengths */
      /* up to multiples of 4 (or 2 for older fonts), and we must round */
      /* up again after stripping off the instructions, it would be */
      /* possible otherwise to have more than 4 bytes of padding which */
      /* is more or less invalid. */

      if (num_contours < 0)
      {
        error = TA_glyph_parse_composite(glyph, buf, len);
        if (error)
          return error;
      }
      else
      {
        error = TA_glyph_parse_simple(glyph, buf, num_contours, len);
        if (error)
          return error;
      }
    }
  }

  return TA_Err_Ok;
}


static FT_Error
TA_sfnt_build_glyf_table(SFNT* sfnt,
                         FONT* font)
{
  SFNT_Table* glyf_table = &font->tables[sfnt->glyf_idx];
  glyf_Data* data = (glyf_Data*)glyf_table->data;

  FT_ULong len;
  FT_Byte* buf_new;
  FT_Byte* p;
  FT_UShort i;


  if (glyf_table->recreated)
    return TA_Err_Ok;

  /* get table size */
  len = 0;
  for (i = 0; i < data->num_glyphs; i++)
  {
    /* glyph records should be aligned to start at multiples of 4 */
    len = (len + 3) & ~3;
    len += data->glyphs[i].len;
  }

  glyf_table->len = len;
  buf_new = (FT_Byte*)realloc(glyf_table->buf, (len + 3) & ~3);
  if (!buf_new)
    return FT_Err_Out_Of_Memory;
  else
    glyf_table->buf = buf_new;

  p = glyf_table->buf;
  for (i = 0; i < data->num_glyphs; i++)
  {
    len = data->glyphs[i].len;
    if (len)
    {
      memcpy(p, data->glyphs[i].buf, len);

      /* pad with zero bytes to make the buffer length a multiple of 4; */
      /* this works even for the last glyph record since */
      /* the `glyf' table length is a multiple of 4 also */
      p += len;
      switch (len % 4)
      {
      case 1:
        *(p++) = 0;
      case 2:
        *(p++) = 0;
      case 3:
        *(p++) = 0;
      default:
        break;
      }
    }
  }

  glyf_table->checksum = TA_table_compute_checksum(glyf_table->buf,
                                                   glyf_table->len);
  glyf_table->recreated = 1;

  return TA_Err_Ok;
}


static FT_Error
TA_sfnt_build_loca_table(SFNT* sfnt,
                         FONT* font)
{
  SFNT_Table* loca_table = &font->tables[sfnt->loca_idx];
  SFNT_Table* glyf_table = &font->tables[sfnt->glyf_idx];
  SFNT_Table* head_table = &font->tables[sfnt->head_idx];
  glyf_Data* data = (glyf_Data*)glyf_table->data;

  FT_ULong offset;
  FT_Byte loca_format;
  FT_Byte* buf_new;
  FT_Byte* p;
  FT_UShort i;


  if (loca_table->recreated)
    return TA_Err_Ok;

  /* get largest offset */
  offset = 0;
  for (i = 0; i < data->num_glyphs; i++)
  {
    /* glyph records should be aligned to start at multiples of 4 */
    offset = (offset + 3) & ~3;
    offset += data->glyphs[i].len;
  }

  if (offset > 0xFFFF * 2)
    loca_format = 1;
  else
    loca_format = 0;

  /* fill table */
  if (loca_format)
  {
    loca_table->len = (data->num_glyphs + 1) * 4;
    buf_new = (FT_Byte*)realloc(loca_table->buf, loca_table->len);
    if (!buf_new)
      return FT_Err_Out_Of_Memory;
    else
      loca_table->buf = buf_new;

    p = loca_table->buf;
    offset = 0;

    for (i = 0; i < data->num_glyphs; i++)
    {
      offset = (offset + 3) & ~3;

      *(p++) = BYTE1(offset);
      *(p++) = BYTE2(offset);
      *(p++) = BYTE3(offset);
      *(p++) = BYTE4(offset);

      offset += data->glyphs[i].len;
    }

    /* last element is the size of the `glyf' table */
    *(p++) = BYTE1(offset);
    *(p++) = BYTE2(offset);
    *(p++) = BYTE3(offset);
    *(p++) = BYTE4(offset);
  }
  else
  {
    loca_table->len = (data->num_glyphs + 1) * 2;
    buf_new = (FT_Byte*)realloc(loca_table->buf,
                                (loca_table->len + 3) & ~3);
    if (!buf_new)
      return FT_Err_Out_Of_Memory;
    else
      loca_table->buf = buf_new;

    p = loca_table->buf;
    offset = 0;

    for (i = 0; i < data->num_glyphs; i++)
    {
      offset = (offset + 1) & ~1;

      *(p++) = HIGH(offset);
      *(p++) = LOW(offset);

      offset += (data->glyphs[i].len + 1) >> 1;
    }

    /* last element is the size of the `glyf' table */
    *(p++) = HIGH(offset);
    *(p++) = LOW(offset);

    /* pad `loca' table to make its length a multiple of 4 */
    if (loca_table->len % 4 == 2)
    {
      *(p++) = 0;
      *(p++) = 0;
    }
  }

  loca_table->checksum = TA_table_compute_checksum(loca_table->buf,
                                                   loca_table->len);
  loca_table->recreated = 1;

  head_table->buf[LOCA_FORMAT_OFFSET] = loca_format;

  return TA_Err_Ok;
}


static FT_Error
TA_sfnt_update_maxp_table(SFNT* sfnt,
                          FONT* font)
{
  SFNT_Table* maxp_table = &font->tables[sfnt->maxp_idx];
  FT_Byte* buf = maxp_table->buf;


  if (maxp_table->recreated)
    return TA_Err_Ok;

  if (maxp_table->len != MAXP_LEN)
    return FT_Err_Invalid_Table;

  buf[MAXP_MAX_ZONES_OFFSET] = 0;
  buf[MAXP_MAX_ZONES_OFFSET + 1] = 1;
  buf[MAXP_MAX_TWILIGHT_POINTS_OFFSET] = 0;
  buf[MAXP_MAX_TWILIGHT_POINTS_OFFSET + 1] = 0;
  buf[MAXP_MAX_STORAGE_OFFSET] = 0;
  buf[MAXP_MAX_STORAGE_OFFSET + 1] = 0;
  buf[MAXP_MAX_FUNCTION_DEFS_OFFSET] = 0;
  buf[MAXP_MAX_FUNCTION_DEFS_OFFSET + 1] = 0;
  buf[MAXP_MAX_INSTRUCTION_DEFS_OFFSET] = 0;
  buf[MAXP_MAX_INSTRUCTION_DEFS_OFFSET + 1] = 0;
  buf[MAXP_MAX_STACK_ELEMENTS_OFFSET] = 0;
  buf[MAXP_MAX_STACK_ELEMENTS_OFFSET + 1] = 0;
  buf[MAXP_MAX_INSTRUCTIONS_OFFSET] = 0;
  buf[MAXP_MAX_INSTRUCTIONS_OFFSET + 1] = 0;

  maxp_table->checksum = TA_table_compute_checksum(maxp_table->buf,
                                                   maxp_table->len);
  maxp_table->recreated = 1;

  return TA_Err_Ok;
}


/* we build a dummy `DSIG' table only */

static FT_Error
TA_table_build_DSIG(FT_Byte** DSIG)
{
  FT_Byte* buf;


  buf = (FT_Byte*)malloc(DSIG_LEN);
  if (!buf)
    return FT_Err_Out_Of_Memory;

  /* version */
  buf[0] = 0x00;
  buf[1] = 0x00;
  buf[2] = 0x00;
  buf[3] = 0x01;

  /* zero signatures */
  buf[4] = 0x00;
  buf[5] = 0x00;

  /* permission flags */
  buf[6] = 0x00;
  buf[7] = 0x00;

  *DSIG = buf;

  return TA_Err_Ok;
}


static void
TA_font_compute_table_offsets(FONT* font,
                              FT_ULong start)
{
  FT_ULong i;
  FT_ULong offset = start;


  for (i = 0; i < font->num_tables; i++)
  {
    SFNT_Table* table = &font->tables[i];


    table->offset = offset;

    /* table offsets must be multiples of 4; */
    /* this also fits the actual buffer lengths */
    offset += (table->len + 3) & ~3;
  }
}


/* If `do_complete' is 0, only return `header_len'. */

static FT_Error
TA_sfnt_build_TTF_header(SFNT* sfnt,
                         FONT* font,
                         FT_Byte** header_buf,
                         FT_ULong* header_len,
                         FT_Int do_complete)
{
  SFNT_Table* tables = font->tables;

  SFNT_Table_Info* table_infos = sfnt->table_infos;
  FT_ULong num_table_infos = sfnt->num_table_infos;

  FT_Byte* buf;
  FT_ULong len;

  FT_Byte* table_record;

  FT_Byte* head_buf = NULL; /* pointer to `head' table */
  FT_ULong head_checksum; /* checksum in `head' table */

  FT_ULong num_tables_in_header;
  FT_ULong i;


  num_tables_in_header = 0;
  for (i = 0; i < num_table_infos; i++)
  {
    /* ignore empty tables */
    if (table_infos[i] != MISSING)
      num_tables_in_header++;
  }

  len = 12 + 16 * num_tables_in_header;
  if (!do_complete)
  {
    *header_len = len;
    return TA_Err_Ok;
  }
  buf = (FT_Byte*)malloc(len);
  if (!buf)
    return FT_Err_Out_Of_Memory;

  /* SFNT version */
  buf[0] = 0x00;
  buf[1] = 0x01;
  buf[2] = 0x00;
  buf[3] = 0x00;

  /* number of tables */
  buf[4] = HIGH(num_tables_in_header);
  buf[5] = LOW(num_tables_in_header);

  /* auxiliary data */
  {
    FT_ULong search_range, entry_selector, range_shift;
    FT_ULong i, j;


    for (i = 1, j = 2; j <= num_tables_in_header; i++, j <<= 1)
      ;

    entry_selector = i - 1;
    search_range = 0x10 << entry_selector;
    range_shift = (num_tables_in_header << 4) - search_range;

    buf[6] = HIGH(search_range);
    buf[7] = LOW(search_range);
    buf[8] = HIGH(entry_selector);
    buf[9] = LOW(entry_selector);
    buf[10] = HIGH(range_shift);
    buf[11] = LOW(range_shift);
  }

  /* location of the first table info record */
  table_record = &buf[12];

  head_checksum = 0;

  /* loop over all tables */
  for (i = 0; i < num_table_infos; i++)
  {
    SFNT_Table_Info table_info = table_infos[i];
    SFNT_Table* table;


    /* ignore empty slots */
    if (table_info == MISSING)
      continue;

    table = &tables[table_info];

    if (table->tag == TTAG_head)
    {
      /* we always reach this IF clause since FreeType would */
      /* have aborted already if the `head' table were missing */

      head_buf = table->buf;

      /* reset checksum in `head' table for recalculation */
      head_buf[8] = 0x00;
      head_buf[9] = 0x00;
      head_buf[10] = 0x00;
      head_buf[11] = 0x00;
    }

    head_checksum += table->checksum;

    table_record[0] = BYTE1(table->tag);
    table_record[1] = BYTE2(table->tag);
    table_record[2] = BYTE3(table->tag);
    table_record[3] = BYTE4(table->tag);

    table_record[4] = BYTE1(table->checksum);
    table_record[5] = BYTE2(table->checksum);
    table_record[6] = BYTE3(table->checksum);
    table_record[7] = BYTE4(table->checksum);

    table_record[8] = BYTE1(table->offset);
    table_record[9] = BYTE2(table->offset);
    table_record[10] = BYTE3(table->offset);
    table_record[11] = BYTE4(table->offset);

    table_record[12] = BYTE1(table->len);
    table_record[13] = BYTE2(table->len);
    table_record[14] = BYTE3(table->len);
    table_record[15] = BYTE4(table->len);

    table_record += 16;
  }

  /* the font header is complete; compute `head' checksum */
  head_checksum += TA_table_compute_checksum(buf, len);
  head_checksum = 0xB1B0AFBAUL - head_checksum;

  /* store checksum in `head' table; */
  head_buf[8] = BYTE1(head_checksum);
  head_buf[9] = BYTE2(head_checksum);
  head_buf[10] = BYTE3(head_checksum);
  head_buf[11] = BYTE4(head_checksum);

  *header_buf = buf;
  *header_len = len;

  return TA_Err_Ok;
}


static FT_Error
TA_font_build_TTF(FONT* font)
{
  SFNT* sfnt = &font->sfnts[0];

  SFNT_Table* tables;
  FT_ULong num_tables;

  FT_ULong SFNT_offset;

  FT_Byte* DSIG_buf;

  FT_Byte* header_buf;
  FT_ULong header_len;

  FT_ULong i;
  FT_Error error;


  /* add a dummy `DSIG' table */

  error = TA_sfnt_add_table_info(sfnt);
  if (error)
    return error;

  error = TA_table_build_DSIG(&DSIG_buf);
  if (error)
    return error;

  /* in case of success, `DSIG_buf' gets linked */
  /* and is eventually freed in `TA_font_unload' */
  error = TA_font_add_table(font,
                            &sfnt->table_infos[sfnt->num_table_infos - 1],
                            TTAG_DSIG, DSIG_LEN, DSIG_buf);
  if (error)
  {
    free(DSIG_buf);
    return error;
  }

  TA_sfnt_sort_table_info(sfnt, font);

  /* the first SFNT table immediately follows the header */
  (void)TA_sfnt_build_TTF_header(sfnt, font, NULL, &SFNT_offset, 0);
  TA_font_compute_table_offsets(font, SFNT_offset);

  error = TA_sfnt_build_TTF_header(sfnt, font,
                                   &header_buf, &header_len, 1);
  if (error)
    return error;

  /* build font */

  tables = font->tables;
  num_tables = font->num_tables;

  font->out_len = tables[num_tables - 1].offset
                  + ((tables[num_tables - 1].len + 3) & ~3);
  font->out_buf = (FT_Byte*)malloc(font->out_len);
  if (!font->out_buf)
  {
    error = FT_Err_Out_Of_Memory;
    goto Err;
  }

  memcpy(font->out_buf, header_buf, header_len);

  for (i = 0; i < num_tables; i++)
  {
    SFNT_Table* table = &tables[i];


    /* buffer length is a multiple of 4 */
    memcpy(font->out_buf + table->offset,
           table->buf, (table->len + 3) & ~3);
  }

  error = TA_Err_Ok;

Err:
  free(header_buf);

  return error;
}


static FT_Error
TA_font_build_TTC_header(FONT* font,
                         FT_Byte** header_buf,
                         FT_ULong* header_len)
{
  SFNT* sfnts = font->sfnts;
  FT_Long num_sfnts = font->num_sfnts;

  SFNT_Table* tables = font->tables;
  FT_ULong num_tables = font->num_tables;

  FT_ULong TTF_offset;
  FT_ULong DSIG_offset;

  FT_Byte* buf;
  FT_ULong len;

  FT_Long i;
  FT_Byte* p;


  len = 24 + 4 * num_sfnts;
  buf = (FT_Byte*)malloc(len);
  if (!buf)
    return FT_Err_Out_Of_Memory;

  p = buf;

  /* TTC ID string */
  *(p++) = 't';
  *(p++) = 't';
  *(p++) = 'c';
  *(p++) = 'f';

  /* TTC header version */
  *(p++) = 0x00;
  *(p++) = 0x02;
  *(p++) = 0x00;
  *(p++) = 0x00;

  /* number of subfonts */
  *(p++) = BYTE1(num_sfnts);
  *(p++) = BYTE2(num_sfnts);
  *(p++) = BYTE3(num_sfnts);
  *(p++) = BYTE4(num_sfnts);

  /* the first TTF subfont header immediately follows the TTC header */
  TTF_offset = len;

  /* loop over all subfonts */
  for (i = 0; i < num_sfnts; i++)
  {
    SFNT* sfnt = &sfnts[i];
    FT_ULong l;


    TA_sfnt_sort_table_info(sfnt, font);
    /* only get header length */
    (void)TA_sfnt_build_TTF_header(sfnt, font, NULL, &l, 0);

    *(p++) = BYTE1(TTF_offset);
    *(p++) = BYTE2(TTF_offset);
    *(p++) = BYTE3(TTF_offset);
    *(p++) = BYTE4(TTF_offset);

    TTF_offset += l;
  }

  /* the first SFNT table immediately follows the subfont TTF headers */
  TA_font_compute_table_offsets(font, TTF_offset);

  /* DSIG tag */
  *(p++) = 'D';
  *(p++) = 'S';
  *(p++) = 'I';
  *(p++) = 'G';

  /* DSIG length */
  *(p++) = 0x00;
  *(p++) = 0x00;
  *(p++) = 0x00;
  *(p++) = 0x08;

  /* DSIG offset; in a TTC this is always the last SFNT table */
  DSIG_offset = tables[num_tables - 1].offset;

  *(p++) = BYTE1(DSIG_offset);
  *(p++) = BYTE2(DSIG_offset);
  *(p++) = BYTE3(DSIG_offset);
  *(p++) = BYTE4(DSIG_offset);

  *header_buf = buf;
  *header_len = len;

  return TA_Err_Ok;
}


static FT_Error
TA_font_build_TTC(FONT* font)
{
  SFNT* sfnts = font->sfnts;
  FT_Long num_sfnts = font->num_sfnts;

  SFNT_Table* tables;
  FT_ULong num_tables;

  FT_Byte* DSIG_buf;
  SFNT_Table_Info dummy;

  FT_Byte* TTC_header_buf;
  FT_ULong TTC_header_len;

  FT_Byte** TTF_header_bufs;
  FT_ULong* TTF_header_lens;

  FT_ULong offset;
  FT_Long i;
  FT_ULong j;
  FT_Error error;


  /* add a dummy `DSIG' table */

  error = TA_table_build_DSIG(&DSIG_buf);
  if (error)
    return error;

  /* in case of success, `DSIG_buf' gets linked */
  /* and is eventually freed in `TA_font_unload' */
  error = TA_font_add_table(font, &dummy, TTAG_DSIG, DSIG_LEN, DSIG_buf);
  if (error)
  {
    free(DSIG_buf);
    return error;
  }

  /* this also computes the SFNT table offsets */
  error = TA_font_build_TTC_header(font,
                                   &TTC_header_buf, &TTC_header_len);
  if (error)
    return error;

  TTF_header_bufs = (FT_Byte**)calloc(1, num_sfnts * sizeof (FT_Byte*));
  if (!TTF_header_bufs)
    goto Err;

  TTF_header_lens = (FT_ULong*)malloc(num_sfnts * sizeof (FT_ULong));
  if (!TTF_header_lens)
    goto Err;

  for (i = 0; i < num_sfnts; i++)
  {
    error = TA_sfnt_build_TTF_header(&sfnts[i], font,
                                     &TTF_header_bufs[i],
                                     &TTF_header_lens[i], 1);
    if (error)
      goto Err;
  }

  /* build font */

  tables = font->tables;
  num_tables = font->num_tables;

  font->out_len = tables[num_tables - 1].offset
                  + ((tables[num_tables - 1].len + 3) & ~3);
  font->out_buf = (FT_Byte*)malloc(font->out_len);
  if (!font->out_buf)
  {
    error = FT_Err_Out_Of_Memory;
    goto Err;
  }

  memcpy(font->out_buf, TTC_header_buf, TTC_header_len);

  offset = TTC_header_len;

  for (i = 0; i < num_sfnts; i++)
  {
    memcpy(font->out_buf + offset,
           TTF_header_bufs[i], TTF_header_lens[i]);

    offset += TTF_header_lens[i];
  }

  for (j = 0; j < num_tables; j++)
  {
    SFNT_Table* table = &tables[j];


    /* buffer length is a multiple of 4 */
    memcpy(font->out_buf + table->offset,
           table->buf, (table->len + 3) & ~3);
  }

  error = TA_Err_Ok;

Err:
  free(TTC_header_buf);
  if (TTF_header_bufs)
  {
    for (i = 0; i < font->num_sfnts; i++)
      free(TTF_header_bufs[i]);
    free(TTF_header_bufs);
  }
  free(TTF_header_lens);

  return error;
}


static FT_Error
TA_font_write(FONT* font,
              FILE* out)
{
  if (fwrite(font->out_buf, 1, font->out_len, out) != font->out_len)
    return TA_Err_Invalid_Stream_Write;

  return TA_Err_Ok;
}


static void
TA_font_unload(FONT* font)
{
  /* in case of error it is expected that unallocated pointers */
  /* are NULL (and counters are zero) */

  if (!font)
    return;

  if (font->tables)
  {
    FT_ULong i;


    for (i = 0; i < font->num_tables; i++)
    {
      free(font->tables[i].buf);
      if (font->tables[i].data)
      {
        if (font->tables[i].tag == TTAG_glyf)
        {
          glyf_Data* data = (glyf_Data*)font->tables[i].data;
          FT_UShort j;


          for (j = 0; j < data->num_glyphs; j++)
            free(data->glyphs[j].buf);
          free(data->glyphs);
          free(data);
        }
      }
    }
    free(font->tables);
  }

  if (font->sfnts)
  {
    FT_Long i;


    for (i = 0; i < font->num_sfnts; i++)
    {
      FT_Done_Face(font->sfnts[i].face);
      free(font->sfnts[i].table_infos);
    }
    free(font->sfnts);
  }

  FT_Done_FreeType(font->lib);
  free(font->in_buf);
  free(font->out_buf);
  free(font);
}


TA_Error
TTF_autohint(FILE* in,
             FILE* out)
{
  FONT* font;
  FT_Error error;
  FT_Long i;


  font = (FONT*)calloc(1, sizeof (FONT));
  if (!font)
    return FT_Err_Out_Of_Memory;

  error = TA_font_read(font, in);
  if (error)
    goto Err;

  error = TA_font_init(font);
  if (error)
    goto Err;

  /* loop over subfonts */
  for (i = 0; i < font->num_sfnts; i++)
  {
    SFNT* sfnt = &font->sfnts[i];


    error = FT_New_Memory_Face(font->lib, font->in_buf, font->in_len,
                               i, &sfnt->face);
    if (error)
      goto Err;

    error = TA_sfnt_split_into_SFNT_tables(sfnt, font);
    if (error)
      goto Err;

    error = TA_sfnt_split_glyf_table(sfnt, font);
    if (error)
      goto Err;
  }

  /* compute global hints */
  /* build `fpgm' table */
  /* build `prep' table */
  /* build `cvt ' table */

  /* handle all glyphs in a loop */
    /* hint the glyph */
    /* build bytecode */

  /* adjust `maxp' table */

  /* loop again over subfonts */
  for (i = 0; i < font->num_sfnts; i++)
  {
    SFNT* sfnt = &font->sfnts[i];


    error = TA_sfnt_build_glyf_table(sfnt, font);
    if (error)
      goto Err;
    error = TA_sfnt_build_loca_table(sfnt, font);
    if (error)
      goto Err;
    error = TA_sfnt_update_maxp_table(sfnt, font);
    if (error)
      goto Err;
  }

  if (font->num_sfnts == 1)
    error = TA_font_build_TTF(font);
  else
    error = TA_font_build_TTC(font);
  if (error)
    goto Err;

  error = TA_font_write(font, out);
  if (error)
    goto Err;

  error = TA_Err_Ok;

Err:
  TA_font_unload(font);

  return error;
}

/* end of ttfautohint.c */
