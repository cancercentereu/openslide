/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2022 Nico Curti
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

/*
 * Olympus (vsi, ets, ome-tif) support
 *
 * quickhash comes from _openslide_tifflike_init_properties_and_hash
 *
 */


#ifndef CMAKE_BUILD
  #include <config.h>
#endif

#include "openslide-private.h"
#include "openslide-decode-jpeg.h"
#include "openslide-decode-jp2k.h"
#include "openslide-decode-png.h"
#include "openslide-decode-xml.h"
#include "openslide-decode-tiff.h"
#include "openslide-decode-tifflike.h"
#include "openslide-decode-gdkpixbuf.h"

#include <png.h>
#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <tiffio.h>
#include <setjmp.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

static const char ETS_EXT[] = ".ets";
static const char TIF_EXT[] = ".tif";
static const char VSI_EXT[] = ".vsi";
static const char SLIDEDATA_DIRNAME[] = "_%s_";
static const char SIS_MAGIC[4] = "SIS";
static const char ETS_MAGIC[4] = "ETS";

#define PARSE_INT_ATTRIBUTE_OR_FAIL(NODE, NAME, OUT)    \
  do {                \
    GError *tmp_err = NULL;         \
    OUT = _openslide_xml_parse_int_attr(NODE, NAME, &tmp_err);  \
    if (tmp_err)  {           \
      g_propagate_error(err, tmp_err);        \
      goto FAIL;            \
    }               \
  } while (0)

#define PARSE_DBL_ATTRIBUTE_OR_FAIL(NODE, NAME, OUT)    \
  do {                \
    GError *tmp_err = NULL;         \
    OUT = _openslide_xml_parse_double_attr(NODE, NAME, &tmp_err);  \
    if (tmp_err)  {           \
      g_propagate_error(err, tmp_err);        \
      goto FAIL;            \
    }               \
  } while (0)

enum image_format {
  FORMAT_RAW,           // 0
  FORMAT_UNKNOWN,       // 1
  FORMAT_JPEG,          // 2
  FORMAT_JP2,           // 3
  FORMAT_UNKNOWN2,      // 4
  FORMAT_JPEG_LOSSLESS, // 5
  FORMAT_UNKNOWN3,      // 6
  FORMAT_UNKNOWN4,      // 7
  FORMAT_PNG,           // 8
  FORMAT_BMP,           // 9
};

enum pixel_types {
  PIXEL_TYPE_UNKNOWN,       // 0
  PIXEL_TYPE_UNKNOWN2,      // 1
  PIXEL_TYPE_UINT8,         // 2
  PIXEL_TYPE_UNKNOWN3,      // 3
  PIXEL_TYPE_INT32,         // 4
};

enum color_space_types {
  COLORSPACE_UNKNOWN,       // 0
  COLORSPACE_FLUORESCENCE,  // 1
  COLORSPACE_UNKNOWN2,      // 2
  COLORSPACE_UNKNOWN3,      // 3
  COLORSPACE_BRIGHTFIELD,   // 4
  COLORSPACE_UNKNOWN4,      // 5
  COLORSPACE_UNKNOWN5,      // 6
};

enum channel_types {
  CHANNEL_UNKNOWN,          // 0
  CHANNEL_GRAYSCALE,        // 1
  CHANNEL_UNKNOWN2,         // 2
  CHANNEL_RGB,              // 3
};

struct sis_header {
  char magic[4]; // SIS0
  uint32_t headerSize;
  uint32_t version;
  uint32_t Ndim;
  uint64_t etsoffset;
  uint32_t etsnbytes;
  uint32_t dummy0; // reserved
  uint64_t offsettiles;
  uint32_t ntiles;
  uint32_t dummy1; // reserved
  uint32_t dummy2; // reserved
  uint32_t dummy3; // reserved
  uint32_t dummy4; // reserved
  uint32_t dummy5; // reserved
};

struct ets_header {
  char magic[4]; // ETS0
  uint32_t version;
  uint32_t pixelType;
  uint32_t sizeC;
  uint32_t colorspace;
  uint32_t compression;
  uint32_t quality;
  uint32_t dimx;
  uint32_t dimy;
  uint32_t dimz;
  uint8_t backgroundColor[3];
  bool usePyramid;
};

struct tile {
  uint32_t dummy1;
  uint32_t coord[3];
  uint32_t level;
  uint64_t offset;
  uint32_t numbytes;
  uint32_t dummy2;
};

struct load_state {
  int32_t w;
  int32_t h;
  GdkPixbuf *pixbuf;  // NULL until validated, then a borrowed ref
  GError *err;
};

struct level {
  struct _openslide_level base;
  struct _openslide_tiff_level *tiffl;
  struct _openslide_grid *grid;

  enum image_format image_format;
  int32_t image_width;
  int32_t image_height;

  double tile_w;
  double tile_h;
  int32_t tile_ch;

  uint32_t current_lvl;
};

struct ome_tiff_ops_data {
  struct _openslide_tiffcache *tc;
};

struct png_error_ctx {
  jmp_buf env;
  GError *err;
};

enum slide_format {
  SLIDE_FMT_UNKNOWN,
  SLIDE_FMT_ETS,
  SLIDE_FMT_TIF
};


// TIFF obj

struct lightsource {
  char *manufacturer;
  char *model;
};

struct channel {
  char *name;
  int32_t emission_wavelength;
  int32_t color;
};

struct image {
  char *creation_date;
  int32_t sizeX;
  int32_t sizeY;
  double mpp_x;
  double mpp_y;
  struct channel *ch;
  double *exposuretime;
};

struct tiff_image_desc {
  char *mycroscope_manufacturer;
  char *mycroscope_model;
  int32_t channels;
  int32_t levels;
  struct lightsource *lightsources;
  struct image *img;
};


static char *_get_parent_image_file(const char *filename,
                                   GError **err) {
  // verify original VSI file in parent directory tree

  char *stackdir = g_path_get_dirname(filename);
  char *imagedir = g_path_get_dirname(stackdir);

  char *basename_ = g_path_get_basename(imagedir);
  char *basename = basename_ + 1; // removes first character
  basename[strlen(basename) - 1] = '\0'; // removes last character

  imagedir = g_path_get_dirname(imagedir);

  char *vsifile_ = g_build_filename(imagedir, basename, NULL);
  char *vsifile = g_strconcat(vsifile_, VSI_EXT, NULL);

  return vsifile;
}

static enum slide_format _get_related_image_file(const char *filename, char **image_filename,
                                    GError **err) {
  // verify slidedat ETS or TIFF exists

  char *basename = g_path_get_basename(filename);
  basename = g_strndup(basename, strlen(basename) - strlen(VSI_EXT));

  char *dirname = g_strndup(filename, strlen(filename) - strlen(VSI_EXT) - strlen(basename));
  char *slidedat_dir = g_strdup_printf(SLIDEDATA_DIRNAME, basename);
  char *slidedat_path = g_build_filename(dirname, slidedat_dir, NULL);

  g_free(basename);
  g_free(dirname);
  g_free(slidedat_dir);

  char *best_file = NULL;
  size_t best_file_size = 0;


  // list all files in directory
  GDir *dir;
  const gchar *slide_dir;

  dir = g_dir_open(slidedat_path, 0, err);
  while ((slide_dir = g_dir_read_name(dir))) {

    // check directory name - if doesn't start from stack1 or is exactly stack1, skip
    if (strncmp(slide_dir, "stack1", 6) < 0 || strlen(slide_dir) == 6)
      continue;

    char *data_dir = g_build_filename(slidedat_path, slide_dir, NULL);
    char *current_file = NULL;

    GDir *nested_dir;
    const gchar *ets_or_tif_file;

    nested_dir = g_dir_open(data_dir, 0, err);
    while ((ets_or_tif_file = g_dir_read_name(nested_dir))) {

      // check file starts with 'frame_t' chars
      if (strncmp(ets_or_tif_file, "frame_t", 7) >= 0) {

        current_file = g_build_filename(data_dir, ets_or_tif_file, NULL);

        bool is_valid = _openslide_fexists(current_file, err);

        if (is_valid) {
          _openslide_file *current_file_ptr = _openslide_fopen(current_file, err);
          if(current_file_ptr) {
            // check file size
            size_t current_file_size = _openslide_fsize(current_file_ptr, err);

            if (current_file_size > best_file_size) {
              best_file = current_file;
              best_file_size = current_file_size;
            } else {
              g_free(current_file);
            }
            _openslide_fclose(current_file_ptr);
          }
        }
      }
    }
    g_free(data_dir);
    g_dir_close(nested_dir);
  }

  g_dir_close(dir);

  g_free(slidedat_path);

  printf("VSI stack used: %s\n", best_file);

  if (best_file && g_str_has_suffix(best_file, ETS_EXT)) {
    *image_filename = best_file;
    return SLIDE_FMT_ETS;

  } else if (best_file && g_str_has_suffix(best_file, TIF_EXT)) {
    *image_filename = best_file;
    g_free(best_file);
    
    return SLIDE_FMT_TIF;

  } else {
    g_free(slidedat_path);
    g_free(best_file);
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Impossible to find related image file");
    return SLIDE_FMT_UNKNOWN;
  }

  return SLIDE_FMT_UNKNOWN;
}


static bool olympus_ets_detect(const char *filename,
                               struct _openslide_tifflike *tl, GError **err) {
  // reject TIFFs
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Is a TIFF file");
    return false;
  }

  // verify filename
  if (!g_str_has_suffix(filename, ETS_EXT)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not have %s extension", ETS_EXT);
    return false;
  }

  // verify existence
  if (!_openslide_fexists(filename, err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not exist");
    return false;
  }

  return true;
}

static bool olympus_tif_detect(const char *filename,
                               struct _openslide_tifflike *tl, GError **err) {
  // reject TIFFs
  if (!tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a TIFF file");
    return false;
  }

  // ensure TIFF is tiled
  if (!_openslide_tifflike_is_tiled(tl, 0)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "TIFF is not tiled");
    return false;
  }

  // verify filename
  if (!g_str_has_suffix(filename, TIF_EXT)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not have %s extension", TIF_EXT);
    return false;
  }

  // verify existence
  if (!_openslide_fexists(filename, err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not exist");
    return false;
  }

  // check xml properties

  // get image description
  const char *image_desc = _openslide_tifflike_get_buffer(tl, 0,
                                                          TIFFTAG_IMAGEDESCRIPTION,
                                                          err);
  if (!image_desc) {
    return false;
  }

  // try to parse the xml
  xmlDoc *doc = _openslide_xml_parse(image_desc, err);
  if (doc == NULL) {
    return false;
  }

  // create XPATH context to query the document
  xmlXPathContext *ctx = NULL;
  ctx = _openslide_xml_xpath_create(doc);

  // get number of planes
  char *username = _openslide_xml_xpath_get_string(ctx, "/d:OME/d:Experimenter/@UserName");
  if (strcmp(username, "olympus")){
    return false;
  }

  return true;
}

static bool olympus_vsi_detect(const char *filename G_GNUC_UNUSED,
                               struct _openslide_tifflike *tl, GError **err) {
#ifndef DEBUG
  // disable warnings
  TIFFSetWarningHandler(NULL);
#endif

  // check if ETS file -> OK
  if (g_str_has_suffix(filename, ETS_EXT)) {

    GError *tmp_err = NULL;
    struct _openslide_tifflike *tl_tif = _openslide_tifflike_create(filename, &tmp_err);
    bool ok_ets = olympus_ets_detect(filename, tl_tif, err);
    _openslide_tifflike_destroy(tl_tif);
    return ok_ets;

  } else if (g_str_has_suffix(filename, TIF_EXT)) { // otherwise it could be a tiff

    GError *tmp_err = NULL;
    struct _openslide_tifflike *tl_tif = _openslide_tifflike_create(filename, &tmp_err);
    bool ok_tif = olympus_tif_detect(filename, tl_tif, err);
    _openslide_tifflike_destroy(tl_tif);
    return ok_tif;

  } else {
    // continue since it could be an original VSI folder tree
  }

  // if it is not neither a VSI raise error
  if (!g_str_has_suffix(filename, VSI_EXT)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not have %s extension", VSI_EXT);
    return false;
  }

  // verify existence
  if (!_openslide_fexists(filename, err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not exist");
    return false;
  }

  // reject TIFFs
  if (!tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a TIFF file");
    return false;
  }

  // ensure TIFF is not tiled
  if (_openslide_tifflike_is_tiled(tl, 0)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "TIFF is tiled");
    return false;
  }

  // verify slidedat ETS or TIFF exists
  char *slidedat_file = NULL;
  enum slide_format fmt = _get_related_image_file(filename, &slidedat_file, err);

  switch (fmt) {
    case SLIDE_FMT_ETS: {
      GError *tmp_err = NULL;
      struct _openslide_tifflike *tl_tif = _openslide_tifflike_create(slidedat_file, &tmp_err);
      bool ok_ets = olympus_ets_detect(slidedat_file, tl_tif, err);
      _openslide_tifflike_destroy(tl_tif);
      //g_free(slidedat_file);
      return ok_ets;
    } break;
    case SLIDE_FMT_TIF: {
      GError *tmp_err = NULL;
      struct _openslide_tifflike *tl_tif = _openslide_tifflike_create(slidedat_file, &tmp_err);
      bool ok_tif = olympus_tif_detect(slidedat_file, tl_tif, err);
      _openslide_tifflike_destroy(tl_tif);
      //g_free(slidedat_file);
      return ok_tif;
    } break;
    default: {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Corresponding slidedat file does not exist");
      return false;
    } break;
  }

  return true;
}

static bool sis_header_read(struct sis_header * self, _openslide_file * stream, GError **err) {
  int check G_GNUC_UNUSED = 0;
  check = _openslide_fread(stream, self->magic, sizeof(self->magic));
  g_assert((strncmp(self->magic, SIS_MAGIC, 4) == 0));
  check = _openslide_fread(stream, (char*)&self->headerSize, sizeof(self->headerSize));
  g_assert((self->headerSize == 64)); // size of struct
  check = _openslide_fread(stream, (char*)&self->version, sizeof(self->version));
  //g_assert((self->version == 2)); // version ??
  check = _openslide_fread(stream, (char*)&self->Ndim, sizeof(self->Ndim));
  g_assert(((self->Ndim == 4) || (self->Ndim == 6))); // dim ?
  check = _openslide_fread(stream, (char*)&self->etsoffset, sizeof(self->etsoffset));
  g_assert((self->etsoffset == 64)); // offset of ETS struct
  check = _openslide_fread(stream, (char*)&self->etsnbytes, sizeof(self->etsnbytes));
  g_assert((self->etsnbytes == 228)); // size of ETS struct
  check = _openslide_fread(stream, (char*)&self->dummy0, sizeof(self->dummy0));
  //g_assert((self->dummy0 == 0)); // ??
  check = _openslide_fread(stream, (char*)&self->offsettiles, sizeof(self->offsettiles)); // offset to tiles
  check = _openslide_fread(stream, (char*)&self->ntiles, sizeof(self->ntiles)); // number of tiles
  check = _openslide_fread(stream, (char*)&self->dummy1, sizeof(self->dummy1)); // ??
  //g_assert((self->dummy1 == 0)); // always zero ?
  check = _openslide_fread(stream, (char*)&self->dummy2, sizeof(self->dummy2)); // some kind of offset ?
  //g_assert((dummy2 == 0)); // not always
  check = _openslide_fread(stream, (char*)&self->dummy3, sizeof(self->dummy3));
  //g_assert((self->dummy3 == 0)); // always zero ?
  check = _openslide_fread(stream, (char*)&self->dummy4, sizeof(self->dummy4));
  //g_assert((dummy4 == 0)); // not always
  check = _openslide_fread(stream, (char*)&self->dummy5, sizeof(self->dummy5));
  //g_assert((self->dummy5 == 0)); // always zero ?

  return true;
}

static bool ets_header_read(struct ets_header * self, _openslide_file * stream, GError **err) {
  int check G_GNUC_UNUSED = 0;
  check = _openslide_fread(stream, self->magic, sizeof(self->magic));
  g_assert((strncmp(self->magic, ETS_MAGIC, 4) == 0));
  check = _openslide_fread(stream, (char*)&self->version, sizeof(self->version));
  //g_assert((self->version == 0x30001 || self->version == 0x30003)); // some kind of version ?
  check = _openslide_fread(stream, (char*)&self->pixelType, sizeof(self->pixelType));
  g_assert(((self->pixelType == PIXEL_TYPE_UINT8) || (self->pixelType == PIXEL_TYPE_INT32) /* when sis_header->dim == 4 */));
  check = _openslide_fread(stream, (char*)&self->sizeC, sizeof(self->sizeC));
  g_assert(((self->sizeC == CHANNEL_GRAYSCALE) || (self->sizeC == CHANNEL_RGB)));
  check = _openslide_fread(stream, (char*)&self->colorspace, sizeof(self->colorspace));
  g_assert(((self->colorspace == COLORSPACE_BRIGHTFIELD) || (self->colorspace == COLORSPACE_FLUORESCENCE)));
  check = _openslide_fread(stream, (char*)&self->compression, sizeof(self->compression)); // codec
  g_assert(((self->compression == FORMAT_JPEG) || (self->compression == FORMAT_JP2)));
  check = _openslide_fread(stream, (char*)&self->quality, sizeof(self->quality));
  //g_assert( self->quality == 90 || self->quality == 100 ); // some kind of JPEG quality ?
  check = _openslide_fread(stream, (char*)&self->dimx, sizeof(self->dimx));
  //g_assert((self->dimx == 512)); // always tile of 512x512 ?
  check = _openslide_fread(stream, (char*)&self->dimy, sizeof(self->dimy));
  //g_assert((self->dimy == 512)); //
  check = _openslide_fread(stream, (char*)&self->dimz, sizeof(self->dimz));
  g_assert((self->dimz == 1) ); // dimz ?

  self->backgroundColor[0] = 0;
  self->backgroundColor[1] = 0;
  self->backgroundColor[2] = 0;

  uint32_t skip_bytes[17];
  check = _openslide_fread(stream, skip_bytes, sizeof(skip_bytes));

  if (self->pixelType == PIXEL_TYPE_UINT8) {

    uint8_t *backgroundColor = (uint8_t*)malloc(sizeof(uint8_t) * self->sizeC);
    check = _openslide_fread(stream, backgroundColor, sizeof(uint8_t) * self->sizeC);

    for (int i = 0; i < (int)self->sizeC; ++i) {
      self->backgroundColor[i] = (uint8_t)(backgroundColor[i]);
    }

    free(backgroundColor);

  } else if (self->pixelType == PIXEL_TYPE_INT32) {

    int32_t *backgroundColor = (int32_t*)malloc(sizeof(int32_t) * self->sizeC);
    check = _openslide_fread(stream, backgroundColor, sizeof(int32_t) * self->sizeC);

    for (int i = 0; i < (int)self->sizeC; ++i) {
      self->backgroundColor[i] = (uint8_t)(backgroundColor[i]);
    }

    free(backgroundColor);
  }

  uint32_t *skip_bytes2 = (uint32_t*)malloc(sizeof(uint32_t) * (10 - self->sizeC));
  check = _openslide_fread(stream, skip_bytes2, sizeof(uint32_t) * (10 - self->sizeC)); // background color

  uint32_t skip_bytes3;
  check = _openslide_fread(stream, (char*)&skip_bytes3, sizeof(skip_bytes3)); // component order

  int32_t usePyramid = 0;
  check = _openslide_fread(stream, (char*)&usePyramid, sizeof(usePyramid)); // use pyramid

  self->usePyramid = usePyramid != 0;

  free(skip_bytes2);

  return true;
}

static void tile_read(struct tile * self, _openslide_file * stream) {
  int check G_GNUC_UNUSED = 0;

  check = _openslide_fread(stream, (char*)&self->dummy1, sizeof(self->dummy1));
  check = _openslide_fread(stream, (char*)self->coord, sizeof(self->coord));
  check = _openslide_fread(stream, (char*)&self->level, sizeof(self->level));
  check = _openslide_fread(stream, (char*)&self->offset, sizeof(self->offset));
  check = _openslide_fread(stream, (char*)&self->numbytes, sizeof(self->numbytes));
  check = _openslide_fread(stream, (char*)&self->dummy2, sizeof(self->dummy2));
}

static struct tile *findtile(struct tile *tiles, uint32_t ntiles, uint32_t x, uint32_t y, uint32_t channel, uint32_t lvl) {
  //const struct tile *ret = NULL;
  for (uint32_t n = 0; n < ntiles; ++n) {
    struct tile * t = tiles + n;
    if( (t->level == lvl) && (t->coord[0] == x && t->coord[1] == y && t->coord[2] == channel) ) {
      return t;
    }
  }
  return NULL;
}

struct olympus_ops_data {
  struct tile *tiles;
  const gchar *datafile_path;
  int32_t num_tiles;
};

static uint32_t *read_ets_image(openslide_t *osr,
                                struct tile * t,
                                enum image_format format,
                                int w, int h,
                                GError **err
                                ) {
  struct olympus_ops_data *data = osr->data;
  const gchar *filename = data->datafile_path;

  //int32_t num_tiles = data->num_tiles;

  uint32_t *dest = g_slice_alloc0(w * h * sizeof(uint32_t));

  _openslide_file *f = _openslide_fopen(filename, err);

  bool result = false;

  // read buffer of data
  _openslide_fseek(f, t->offset, SEEK_SET, err);
  int32_t buflen = t->numbytes / sizeof(uint8_t);

  void * buffer = g_slice_alloc(buflen);
  int32_t check = _openslide_fread(f, buffer, t->numbytes);
  if (!check)
    goto FAIL;

  switch (format) {
  case FORMAT_JPEG:
    result = _openslide_jpeg_decode_buffer(buffer, buflen,
                                           dest,
                                           w, h,
                                           err);
    break;
  case FORMAT_JP2:
    result = _openslide_jp2k_decode_buffer(dest,
                                           w, h,
                                           buffer, buflen,
                                           -1,
                                           err);
    break;
  //case FORMAT_PNG:
  //  result = _openslide_png_decode_buffer(buffer, buflen,
  //                                        dest,
  //                                        w, h,
  //                                        err);
  //  break;
  //case FORMAT_BMP:
  //  result = _openslide_gdkpixbuf_decode_buffer(buffer, buflen,
  //                                              dest,
  //                                              w, h,
  //                                              err);
  //  break;
  default:
    g_assert_not_reached();
  }

  g_free(buffer);
  _openslide_fclose(f);

  if (!result)
    goto FAIL;

  return dest;

FAIL:

  g_slice_free1(w * h * 4, dest);
  return NULL;
}


static bool read_ets_tile(openslide_t *osr,
                          cairo_t *cr,
                          struct _openslide_level *level,
                          int64_t tile_col, int64_t tile_row, /* int64_t tile_channel, // i.e TileIdx_x, TileIdx_y */
                          void *arg G_GNUC_UNUSED,
                          GError **err) {
  int64_t tile_channel = 0;
                      
  struct level *l = (struct level *) level;
  struct olympus_ops_data *data = osr->data;
  struct tile *tiles = data->tiles;

  if (tile_channel > l->tile_ch) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid tile channel %"PRId64, tile_channel);
    return false;
  }

  // TODO: Now we are keeping only the 1st channel!!
  struct tile *t = findtile(tiles, data->num_tiles, tile_col, tile_row, tile_channel, l->current_lvl);
  bool success = true;

  int32_t iw = l->image_width; // Tilew
  int32_t ih = l->image_height; // Tileh

  // get the image data, possibly from cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row, /* tile_channel, */
                                            &cache_entry);

  if (t && !tiledata) {
    tiledata = read_ets_image(osr, t, l->image_format, iw, ih, err);

    if (tiledata == NULL) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Cannot read tile %"PRId64" %"PRId64" %"PRId64,
                  tile_col, tile_row, tile_channel);
      return false;
    }

    _openslide_cache_put(osr->cache,
                         level, tile_col, tile_row, /* tile_channel, */
                         tiledata,
                         iw * ih * 4,
                         &cache_entry);
  }

  if(tiledata) {
    // draw it
    cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                                  CAIRO_FORMAT_RGB24,
                                                                  iw, ih,
                                                                  iw * 4);

    // if we are drawing a subregion of the tile, we must do an additional copy,
    // because cairo lacks source clipping
    if ((l->image_width > l->tile_w) ||
        (l->image_height > l->tile_h)) {
      cairo_surface_t *surface2 = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                            ceil(l->tile_w),
                                                            ceil(l->tile_h));
      cairo_t *cr2 = cairo_create(surface2);
      cairo_set_source_surface(cr2, surface, t->coord[0], t->coord[1]);

      // replace original image surface
      cairo_surface_destroy(surface);
      surface = surface2;

      cairo_rectangle(cr2, 0, 0,
                      ceil(l->tile_w),
                      ceil(l->tile_h));
      cairo_fill(cr2);
      success = _openslide_check_cairo_status(cr2, err);
      cairo_destroy(cr2);
    }

    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_surface_destroy(surface);
    cairo_paint(cr);

    // done with the cache entry, release it
    _openslide_cache_entry_unref(cache_entry);
  }

  return success;
}

static bool paint_ets_region(openslide_t *osr G_GNUC_UNUSED, cairo_t *cr,
                             int64_t x, int64_t y, /* int64_t channel, */
                             struct _openslide_level *level,
                             int32_t w, int32_t h,
                             GError **err) {
  struct level *l = (struct level *) level;

  return _openslide_grid_paint_region(l->grid, cr, NULL,
                                      x / level->downsample,
                                      y / level->downsample,
                                      /* channel, // CHANNEL FOR FLUORESCENCE */
                                      level, w, h,
                                      err);
}

static void destroy_ets(openslide_t *osr) {
  // each level in turn
  for (int32_t i = 0; i < osr->level_count; i++) {
    struct level *l = (struct level *) osr->levels[i];
    _openslide_grid_destroy(l->grid);
    g_slice_free(struct level, l);
  }

  // the level array
  g_free(osr->levels);
}

static const struct _openslide_ops olympus_ets_ops = {
  .paint_region = paint_ets_region,
  .destroy = destroy_ets,
};


int ascending_compare (const void * a, const void * b) {
  return ( *(uint32_t*)b - *(uint32_t*)a );
}


static bool olympus_open_ets(openslide_t *osr, const char *filename,
                             struct _openslide_tifflike *tl,
                             struct _openslide_hash *quickhash1, GError **err) {

  struct tile *tiles = NULL;
  struct level **levels = NULL;
  uint32_t *tilexmax = NULL;
  uint32_t *tileymax = NULL;

  int32_t level_count = 1;
  int32_t channels = 0;

  // open file
  _openslide_file *f = _openslide_fopen(filename, err);
  if (!f) {
    goto FAIL;
  }

  // SIS:
  struct sis_header *sh = g_slice_new0(struct sis_header);
  if (!sis_header_read( sh, f, err )) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Errors in SIS header");
    goto FAIL;
  }

  // ETS:
  struct ets_header *eh = g_slice_new0(struct ets_header);
  if (!ets_header_read( eh, f, err )) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Errors in ETS header");
    goto FAIL;
  }

  // individual tiles
  if (!_openslide_fseek(f, sh->offsettiles, SEEK_SET, err)) {
    _openslide_io_error(err, "Couldn't seek to JPEG start");
    goto FAIL;
  }

  // computes tiles dims
  tiles = g_new0(struct tile, sh->ntiles);
  for (int i = 0; i < (int)sh->ntiles; ++i) {
    struct tile t;
    // read tile images
    tile_read(&t, f);
    tiles[i] = t;

    level_count = (int32_t)t.level > level_count ? (int32_t)t.level : level_count;
    channels = (int32_t)t.coord[2] > channels ? (int32_t)t.coord[2] : channels;
  }

  ++level_count;
  channels = channels != 0 ? channels + 1 : channels;

  // close the input file
  _openslide_fclose(f);

  tilexmax = g_new0(uint32_t, level_count);
  tileymax = g_new0(uint32_t, level_count);

  for (int i = 0; i < (int)sh->ntiles; ++i) {
    struct tile t = tiles[i];
    uint32_t lvl = t.level;

    tilexmax[lvl] = t.coord[0] > tilexmax[lvl] ? t.coord[0] : tilexmax[lvl];
    tileymax[lvl] = t.coord[1] > tileymax[lvl] ? t.coord[1] : tileymax[lvl];
  }

  // sort dimensions
  // NOTE: this is a sanity check to ensure the correct order of levels
  // in the dimension evaluation.
  // In the next loop for level creation, we will assume that the order
  // of levels follows the HR -> LR order (ref check about index in image
  // width and height).
  qsort(tilexmax, level_count, sizeof(uint32_t), ascending_compare);
  qsort(tileymax, level_count, sizeof(uint32_t), ascending_compare);

  levels = g_new0(struct level *, level_count);

  uint32_t image_width = 0;
  uint32_t image_height = 0;

  for (int i = 0; i < level_count; ++i) {
    struct level *l = g_slice_new0(struct level);

    // compute image info:

    // NOTE: we are assuming that each level is exactly the 2x of
    // the previous one. The information about the correct downsampling
    // were not found in the ETS header...
    // This is just a brute force hacking to provide the correct dimensions
    // of images for low level images
    image_width = i == 0 ? eh->dimx * tilexmax[i] : ceil(image_width / 2);
    image_height = i == 0 ? eh->dimy * tileymax[i] : ceil(image_height / 2);

    // TODO: It works ONLY for image without z-stack!
    g_assert( eh->dimz == 1 );

    uint32_t tile_w = eh->dimx;
    uint32_t tile_h = eh->dimy;

    uint32_t tile_across = (image_width + tile_w - 1) / tile_w;
    uint32_t tile_down = (image_height + tile_h - 1) / tile_h;
    //uint32_t tiles_per_image = tile_across * tile_down;

    levels[i] = l;

    l->tile_w = (double)tile_w;
    l->tile_h = (double)tile_h;
    l->tile_ch = channels;
    l->base.w = (double)image_width;
    l->base.h = (double)image_height;
    l->image_format = eh->compression;
    l->image_width = eh->dimx;
    l->image_height = eh->dimy;
    l->current_lvl = i;

    // NOTE: the assumption about image downsampling affects also
    // this line
    l->base.downsample = pow(2., i);

    l->grid = _openslide_grid_create_simple(osr,
                                            tile_across, tile_down,
                                            tile_w, tile_h,
                                            read_ets_tile);
  }

  g_free(tilexmax);
  g_free(tileymax);

  _openslide_set_bounds_props_from_grid(osr, levels[0]->grid);

  osr->level_count = level_count;
  // osr->plane_count = channels == 0 ? 1 : channels;
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  levels = NULL;

  // set private data
  g_assert(osr->data == NULL);
  struct olympus_ops_data *data = g_slice_new0(struct olympus_ops_data);
  data->tiles = tiles;
  data->num_tiles = sh->ntiles;
  data->datafile_path = filename;

  tiles = NULL;
  osr->data = data;

  osr->ops = &olympus_ets_ops;

  // set background property
  _openslide_set_background_color_prop(osr,
    eh->backgroundColor[0], eh->backgroundColor[1], eh->backgroundColor[2]);

  return true;

FAIL:
  // free
  if (f) {
    _openslide_fclose(f);
  }

  if (levels != NULL) {
    for (int i = 0; i < level_count; ++i) {
      struct level *l = levels[i];
      if (l) {
        _openslide_grid_destroy(l->grid);
        g_slice_free(struct level, l);
      }
    }
    g_free(levels);
  }

  g_free(tiles);
  g_free(tilexmax);
  g_free(tileymax);

  return false;
}

static int width_compare(gconstpointer a, gconstpointer b) {
  const struct level *la = *(const struct level **) a;
  const struct level *lb = *(const struct level **) b;

  if (la->tiffl[0].image_w > lb->tiffl[0].image_w) {
    return -1;
  } else if (la->tiffl[0].image_w == lb->tiffl[0].image_w) {
    return 0;
  } else {
    return 1;
  }
}

static void destroy_tif(openslide_t *osr) {
  struct ome_tiff_ops_data *data = osr->data;
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct ome_tiff_ops_data, data);

  for (int32_t i = 0; i < osr->level_count; i++) {
    struct level *l = (struct level *) osr->levels[i];
    _openslide_grid_destroy(l->grid);
    g_slice_free(struct level, l);
  }
  g_free(osr->levels);
}

static bool read_tif_tile(openslide_t *osr,
                          cairo_t *cr,
                          struct _openslide_level *level,
                          int64_t tile_col, int64_t tile_row, /* int64_t tile_channel, */
                          void *arg,
                          GError **err) {
  int64_t tile_channel = 0;
  struct level *l = (struct level *) level;
  struct _openslide_tiff_level *tiffl = &l->tiffl[tile_channel];
  TIFF *tiff = arg;

  // tile size
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row, /* tile_channel, */
                                            &cache_entry);
  if (!tiledata) {
    tiledata = g_slice_alloc(tw * th * 4);
    if (!_openslide_tiff_read_tile(tiffl, tiff,
                                   tiledata, tile_col, tile_row,
                                   err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // clip, if necessary
    if (!_openslide_tiff_clip_tile(tiffl, tiledata,
                                   tile_col, tile_row,
                                   err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // put it in the cache
    _openslide_cache_put(osr->cache, level, tile_col, tile_row, /* tile_channel, */
                         tiledata, tw * th * 4,
                         &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                                 CAIRO_FORMAT_ARGB32,
                                                                 tw, th,
                                                                 tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);

  return true;
}

static bool paint_tif_region(openslide_t *osr, cairo_t *cr,
                             int64_t x, int64_t y, /* int64_t channel, */
                             struct _openslide_level *level,
                             int32_t w, int32_t h,
                             GError **err) {
  struct ome_tiff_ops_data *data = osr->data;
  struct level *l = (struct level *) level;

  g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(data->tc, err);
  if (ct.tiff == NULL) {
    return false;
  }

  bool success = _openslide_grid_paint_region(l->grid, cr, ct.tiff,
                                              x / l->base.downsample,
                                              y / l->base.downsample,
                                              /* channel, // CHANNEL FOR FLUORESCENCE */
                                              level, w, h,
                                              err);
  return success;
}

static const struct _openslide_ops ome_tiff_ops = {
  .paint_region = paint_tif_region,
  .destroy = destroy_tif,
};


static struct tiff_image_desc *parse_xml_description(const char *xml,
                                           GError **err) {

  xmlXPathContext *ctx = NULL;
  struct tiff_image_desc *img = NULL;

  // try to parse the xml
  xmlDoc *doc = _openslide_xml_parse(xml, err);
  if (doc == NULL) {
    return NULL;
  }

  // create XPATH context to query the document
  ctx = _openslide_xml_xpath_create(doc);

  // create image struct
  img = g_slice_new0(struct tiff_image_desc);

  img->mycroscope_manufacturer = _openslide_xml_xpath_get_string(ctx, "/d:OME/d:Instrument/d:Microscope/@Manufacturer");
  img->mycroscope_model = _openslide_xml_xpath_get_string(ctx, "/d:OME/d:Instrument/d:Microscope/@Model");

  // get lightsources nodes
  xmlXPathObject *light_result = _openslide_xml_xpath_eval(ctx,
                                                    "/d:OME/d:Instrument/d:LightSource");
  if (!light_result) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't find lightsources element");
    goto FAIL;
  }

  // get luminance information
  img->channels = light_result->nodesetval->nodeNr;
  img->lightsources = g_new(struct lightsource, img->channels);

  for (int i = 0; i < img->channels; ++i) {
    xmlNode *light_node = light_result->nodesetval->nodeTab[i];

    // get manufacturer node
    xmlChar *xmlstr = xmlGetProp(light_node, BAD_CAST "Manufacturer");
    img->lightsources[i].manufacturer = g_strdup((char *) xmlstr);

    xmlstr = xmlGetProp(light_node, BAD_CAST "Model");
    img->lightsources[i].model = g_strdup((char *) xmlstr);

    xmlFree(xmlstr);
  }

  // get image nodes
  xmlXPathObject *images_result = _openslide_xml_xpath_eval(ctx,
                                                     "/d:OME/d:Image");
  if (!images_result) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't find images element");
    goto FAIL;
  }

  img->levels = images_result->nodesetval->nodeNr;
  img->img = g_new(struct image, img->levels);


  for (int i = 0; i < img->levels; ++i) {
    xmlNode *img_node = images_result->nodesetval->nodeTab[i];
    ctx->node = img_node;

    img->img[i].creation_date = _openslide_xml_xpath_get_string(ctx, "d:AcquisitionDate/text()");

    // get view node
    xmlNode *pixels = _openslide_xml_xpath_get_node(ctx, "d:Pixels");
    if (!pixels) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't find pixels node");
      goto FAIL;
    }

    PARSE_INT_ATTRIBUTE_OR_FAIL(pixels, "SizeX", img->img[i].sizeX);
    PARSE_INT_ATTRIBUTE_OR_FAIL(pixels, "SizeY", img->img[i].sizeY);

    PARSE_DBL_ATTRIBUTE_OR_FAIL(pixels, "PhysicalSizeX", img->img[i].mpp_x);
    PARSE_DBL_ATTRIBUTE_OR_FAIL(pixels, "PhysicalSizeY", img->img[i].mpp_y);

    ctx->node = pixels;

    // get view node
    xmlXPathObject *channels = _openslide_xml_xpath_eval(ctx, "d:Channel");
    if (!channels) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't find channels node");
      goto FAIL;
    }

    img->img[i].ch = g_new(struct channel, channels->nodesetval->nodeNr);
    g_assert(img->channels > 0);
    g_assert(channels->nodesetval->nodeNr > 0);

    //g_assert(channels->nodesetval->nodeNr == img->channels); // This is true for fluorescence only
    img->channels = channels->nodesetval->nodeNr;

    for (int j = 0; j < channels->nodesetval->nodeNr; ++j) {
      xmlNode *channel_node = channels->nodesetval->nodeTab[j];

      xmlChar *xmlstr = xmlGetProp(channel_node, BAD_CAST "Name");
      img->img[i].ch[j].name = g_strdup((char *) xmlstr);

      if (xmlHasProp(channel_node, (unsigned char*)"EmissionWavelength")) {
        img->img[i].ch[j].emission_wavelength = _openslide_xml_parse_int_attr(channel_node, "EmissionWavelength", err);
      } else {
        img->img[i].ch[j].emission_wavelength = 0;
      }
      if (xmlHasProp(channel_node, (unsigned char*)"Color")) {
        img->img[i].ch[j].color = _openslide_xml_parse_int_attr(channel_node, "Color", err);
      } else {
        img->img[i].ch[j].color = 0;
      }
    }

    // get view node
    xmlXPathObject *planes = _openslide_xml_xpath_eval(ctx, "d:Plane");
    if (!planes) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't find planes node");
      goto FAIL;
    }

    img->img[i].exposuretime = g_new(double, planes->nodesetval->nodeNr);
    //g_assert(planes->nodesetval->nodeNr == channels->nodesetval->nodeNr);

    for (int j = 0; j < planes->nodesetval->nodeNr; ++j) {
      xmlNode *pln_node = planes->nodesetval->nodeTab[j];
      img->img[i].exposuretime[j] = _openslide_xml_parse_double_attr(pln_node, "ExposureTime", err);
    }
  }

//  printf("----------device model: %s\n", img->mycroscope_manufacturer);
//  printf("----------device version: %s\n", img->mycroscope_model);
//  printf("----------%d lightsource\n", img->channels);
//  for (int i = 0; i < img->channels; ++i) {
//    printf("--------------lightsource %d: manufacturer %s; model %s\n",
//      i, img->lightsources[i].manufacturer, img->lightsources[i].model);
//  }
//  printf("----------%d levels\n", img->levels);
//  for (int i = 0; i < img->levels; ++i) {
//    printf("--------------img %d: acquisition %s; size [%d, %d]; mpp [%f, %f]\n",
//      i, img->img[i].creation_date, img->img[i].sizeX, img->img[i].sizeY,
//      img->img[i].mpp_x, img->img[i].mpp_y);
////    for (int j = 0; j < channels->nodesetval->nodeNr; ++j) {
////      printf("----------------channel %s: emission_wavelength %d; color %d\n",
////        img->img[i].ch[j].name,
////        img->img[i].ch[j].emission_wavelength,
////        img->img[i].ch[j].color);
////    }
////    for (int j = 0; j < channels->nodesetval->nodeNr; ++j) {
////      printf("--------------exposure time %f\n", img->img[i].exposuretime[j]);
////    }
//  }


  return img;

FAIL:
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);

  return NULL;
}

static void set_prop(openslide_t *osr, const char *name, const char *value) {
  if (value) {
    g_hash_table_insert(osr->properties,
                        g_strdup(name),
                        g_strdup(value));
  }
}

static bool olympus_open_tif(openslide_t *osr, const char *filename,
                             struct _openslide_tifflike *tl,
                             struct _openslide_hash *quickhash1, GError **err) {

  GPtrArray *level_array = g_ptr_array_new();

  // open TIFF
  struct _openslide_tiffcache *tc = _openslide_tiffcache_create(filename);
  g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(tc, err);
  TIFF *tiff = ct.tiff;
  if (!tiff) {
    goto FAIL;
  }

  // get image description
  char *image_desc;
  if (!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc)) {
    return false;
  }

  // read XML
  struct tiff_image_desc * img_desc = parse_xml_description(image_desc, err);
  if (!img_desc) {
    goto FAIL;
  }

  // set some properties
  set_prop(osr, "olympus.device-model", img_desc->mycroscope_manufacturer);
  set_prop(osr, "olympus.device-version", img_desc->mycroscope_model);

  for (int i = 0; i < img_desc->levels; ++i) {

    struct level *l = g_slice_new0(struct level);
    l->tiffl = g_new0(struct _openslide_tiff_level, img_desc->channels);

    for (int j = 0; j < img_desc->channels; ++j) {

      struct _openslide_tiff_level *tiffl = &l->tiffl[j];

      // confirm that this directory is tiled
      if (!TIFFIsTiled(tiff)) {
        continue;
      }

      // Override channel-level information

      if (!_openslide_tiff_level_init(tiff,
                                      TIFFCurrentDirectory(tiff),
                                      (struct _openslide_level *) l,
                                      tiffl,
                                      err)) {
        g_slice_free(struct level, l);
        goto FAIL;
      }

      // check level order
      if (j > 0) {
        int32_t w_ = tiffl->image_w;
        int32_t h_ = tiffl->image_h;
        g_assert(w_ == l->tiffl[j - 1].image_w);
        g_assert(h_ == l->tiffl[j - 1].image_h);
      }

      TIFFReadDirectory(tiff);
    }

    l->grid = _openslide_grid_create_simple(osr,
                                            l->tiffl[0].tiles_across,
                                            l->tiffl[0].tiles_down,
                                            l->tiffl[0].tile_w,
                                            l->tiffl[0].tile_h,
                                            read_tif_tile);
    // add to array
    g_ptr_array_add(level_array, l);
  }


  // TODO: add more properties (expecially related to channels)

//  printf("----------%d lightsource\n", img->channels);
//  for (int i = 0; i < img->channels; ++i) {
//    printf("--------------lightsource %d: manufacturer %s; model %s\n",
//      i, img->lightsources[i].manufacturer, img->lightsources[i].model);
//  }
//  printf("----------%d levels\n", img->levels);
//  for (int i = 0; i < img->levels; ++i) {
//    printf("--------------img %d: acquisition %s; size [%d, %d]; mpp [%f, %f]\n",
//      i, img->img[i].creation_date, img->img[i].sizeX, img->img[i].sizeY,
//      img->img[i].mpp_x, img->img[i].mpp_y);
//    for (int j = 0; j < img->channels; ++j) {
//      printf("----------------channel %s: emission_wavelength %d; color %d\n",
//        img->img[i].ch[j].name,
//        img->img[i].ch[j].emission_wavelength,
//        img->img[i].ch[j].color);
//    }
//    for (int j = 0; j < img->channels; ++j) {
//      printf("--------------exposure time %f\n", img->img[i].exposuretime[j]);
//    }
//  }


  // sort tiled levels
  g_ptr_array_sort(level_array, width_compare);

  // unwrap level array
  int32_t level_count = level_array->len;
  struct level **levels =
    (struct level **) g_ptr_array_free(level_array, false);
  level_array = NULL;

  // allocate private data
  struct ome_tiff_ops_data *data = g_slice_new0(struct ome_tiff_ops_data);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  // osr->plane_count = img_desc->channels;
  osr->data = data;
  osr->ops = &ome_tiff_ops;

  // put TIFF handle and store tiffcache reference
  data->tc = tc;

  return true;

 FAIL:
  // free the level array
  if (level_array) {
    for (uint32_t n = 0; n < level_array->len; n++) {
      struct level *l = level_array->pdata[n];
      _openslide_grid_destroy(l->grid);
      g_slice_free(struct level, l);
    }
    g_ptr_array_free(level_array, true);
  }
  // free TIFF
  return false;
}


static void set_resolution_prop(openslide_t *osr, TIFF *tiff,
                                const char *property_name,
                                ttag_t tag) {
  float f;
  uint16_t unit;

  if (TIFFGetFieldDefaulted(tiff, TIFFTAG_RESOLUTIONUNIT, &unit) &&
      TIFFGetField(tiff, tag, &f)) {
    if (unit == RESUNIT_CENTIMETER) {
      g_hash_table_insert(osr->properties, g_strdup(property_name),
                          _openslide_format_double(10000.0 / f));
    }
    else if (unit == RESUNIT_INCH) {
      g_hash_table_insert(osr->properties, g_strdup(property_name),
                          _openslide_format_double(25400.0 / f)); // TODO: correct according to inches
    }
  }
}

static bool olympus_open_vsi(openslide_t *osr, const char *filename,
                         struct _openslide_tifflike *tl,
                         struct _openslide_hash *quickhash1, GError **err) {
  bool success = true;

  if (g_str_has_suffix(filename, ETS_EXT)) {

    // TODO: ETS format does not contain any metadata useful for
    // openslide so some informative properties could be not
    // set using ETS directly.
    // A possible solution could be given by checking the associated
    // .vsi file in the parent directory but we must be sure about
    // the directory order !
    char *imagefile = _get_parent_image_file(filename, err);
    success = _openslide_fexists(imagefile, err);

    if (success) {

      struct _openslide_tiffcache *tc = _openslide_tiffcache_create(imagefile);
      g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(tc, err);
      TIFF *tiff = ct.tiff;
      if (!tiff) {
        goto FAIL;
      }

      uint16_t compression;
      if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Can't read compression scheme");
        goto FAIL;
      };

      if (!TIFFIsCODECConfigured(compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unsupported TIFF compression: %u", compression);
        goto FAIL;
      }

      struct _openslide_tifflike *tl = _openslide_tifflike_create(imagefile,
                                                                  err);

      if (!_openslide_tifflike_init_properties_and_hash(osr, tl, quickhash1,
                                                        0, 0,
                                                        err)) {
        goto FAIL;
      }

      // keep the XML document out of the properties
      // (in case pyramid level 0 is also directory 0)
      g_hash_table_remove(osr->properties, OPENSLIDE_PROPERTY_NAME_COMMENT);
      g_hash_table_remove(osr->properties, "tiff.ImageDescription");

      /*
      // set MPP properties
      if (!_openslide_tiff_set_dir(tiff, 0, err)) {
        goto FAIL;
      }
      set_resolution_prop(osr, tiff, OPENSLIDE_PROPERTY_NAME_MPP_X,
                          TIFFTAG_XRESOLUTION);
      set_resolution_prop(osr, tiff, OPENSLIDE_PROPERTY_NAME_MPP_Y,
                          TIFFTAG_YRESOLUTION);*/

      if (!_openslide_tiff_add_associated_image(osr, "macro", tc,
                                                1, err)) {
        goto FAIL;
      }

      success = olympus_open_ets(osr, filename, tl, quickhash1, err);

      return success;
    } else {
      goto FAIL;
    }

  } else if (g_str_has_suffix(filename, TIF_EXT)) { // otherwise it could be a tiff

    char *imagefile = _get_parent_image_file(filename, err);
    success = _openslide_fexists(imagefile, err);

    if (success) {

      struct _openslide_tiffcache *tc = _openslide_tiffcache_create(imagefile);
      g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(tc, err);
      TIFF *tiff = ct.tiff;
      if (!tiff) {
        goto FAIL;
      }

      uint16_t compression;
      if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Can't read compression scheme");
        goto FAIL;
      };

      if (!TIFFIsCODECConfigured(compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unsupported TIFF compression: %u", compression);
        goto FAIL;
      }

      if (!_openslide_tifflike_init_properties_and_hash(osr, tl, quickhash1,
                                                        0, 0,
                                                        err)) {
        goto FAIL;
      }

      // keep the XML document out of the properties
      // (in case pyramid level 0 is also directory 0)
      g_hash_table_remove(osr->properties, OPENSLIDE_PROPERTY_NAME_COMMENT);
      g_hash_table_remove(osr->properties, "tiff.ImageDescription");

      // set MPP properties
      if (!_openslide_tiff_set_dir(tiff, 0, err)) {
        goto FAIL;
      }
      set_resolution_prop(osr, tiff, OPENSLIDE_PROPERTY_NAME_MPP_X,
                          TIFFTAG_XRESOLUTION);
      set_resolution_prop(osr, tiff, OPENSLIDE_PROPERTY_NAME_MPP_Y,
                          TIFFTAG_YRESOLUTION);

      if (!_openslide_tiff_add_associated_image(osr, "macro", tc,
                                                1, err)) {
        goto FAIL;
      }

      success = olympus_open_tif(osr, filename, tl, quickhash1, err);
      return success;
    } else {
      goto FAIL;
    }

  } else {
    // continue since it could be an original VSI folder tree
  }

  struct _openslide_tiffcache *tc = _openslide_tiffcache_create(filename);
  g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(tc, err);
  TIFF *tiff = ct.tiff;
  if (!tiff) {
    goto FAIL;
  }

  uint16_t compression;
  if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't read compression scheme");
    goto FAIL;
  };
  if (!TIFFIsCODECConfigured(compression)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported TIFF compression: %u", compression);
    goto FAIL;
  }

  if (!_openslide_tifflike_init_properties_and_hash(osr, tl, quickhash1,
                                                    0, 0,
                                                    err)) {
    goto FAIL;
  }

  // keep the XML document out of the properties
  // (in case pyramid level 0 is also directory 0)
  g_hash_table_remove(osr->properties, OPENSLIDE_PROPERTY_NAME_COMMENT);
  g_hash_table_remove(osr->properties, "tiff.ImageDescription");

  /*
  // set MPP properties
  if (!_openslide_tiff_set_dir(tiff, 0, err)) {
    goto FAIL;
  }
  set_resolution_prop(osr, tiff, OPENSLIDE_PROPERTY_NAME_MPP_X,
                      TIFFTAG_XRESOLUTION);
  set_resolution_prop(osr, tiff, OPENSLIDE_PROPERTY_NAME_MPP_Y,
                      TIFFTAG_YRESOLUTION);

  if (!_openslide_tiff_add_associated_image(osr, "macro", tc,
                                            1, err)) {
    goto FAIL;
  }
  */

  // verify slidedat ETS or TIFF exists
  char *slidedat_file = NULL;
  enum slide_format fmt = _get_related_image_file(filename, &slidedat_file, err);

  switch (fmt) {
    case SLIDE_FMT_ETS: {
      success = olympus_open_ets(osr, slidedat_file, tl, quickhash1, err);
    } break;
    case SLIDE_FMT_TIF: {
      success = olympus_open_tif(osr, slidedat_file, tl, quickhash1, err);
    } break;
    default: {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Corresponding slidedat file does not exist");
      goto FAIL;
    } break;
  }

  //g_free(slidedat_file);

  return success;

FAIL:

  success = false;

  return success;
}


const struct _openslide_format _openslide_format_olympus = {
  .name = "olympus-vsi",
  .vendor = "olympus",
  .detect = olympus_vsi_detect,
  .open = olympus_open_vsi,
};
