/*
 * Copyright (C) 2006, 2007 Jean-Baptiste Note <jean-baptiste.note@m4x.org>
 *
 * This file is part of debit.
 *
 * Debit is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Debit is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with debit.  If not, see <http://www.gnu.org/licenses/>.
 */

/** \file
 *
 * Idea to rewrite the DB:
 * Once the control bits are correctly ordered, the bit patterns (IE,
 * pattern values to be checked against) are known. Therefore, one can
 * record positionally the input wire without the bit pattern itself,
 * which is implicit in the relative position of the record.
 *
 * Thus we get rid of the guin32, and directly halve the database
 * space. However, a small 8-bit index may be needed to indicate the
 * type of the pip and associated patterns. This is, however, very
 * small. The whole database (*for all sites*) will certainly fit well
 * under 100KiB, which means it'll fit in L2 with room to spare.
 *
 */

/* implements the functions needed to get pips in the db */

#include <glib.h>
#include "debitlog.h"

#include "keyfile.h"

#include "bitstream.h"
#include "localpips.h"
#include "wiring.h"
#include "design.h"

#include "cfgbit.h"

#ifdef __COMPILED_PIPSDB

/* The data */
#if defined(VIRTEX2)
#include "data/virtex2/pips.h"
#elif defined(VIRTEX4)
#include "data/virtex4/pips.h"
#elif defined(VIRTEX5)
#include "data/virtex5/pips.h"
#elif defined(SPARTAN3)
#include "data/spartan3/pips.h"
#else
#error "Could not compile in pip db"
#endif

/* The initialization functions */
pip_db_t *
get_pipdb(const gchar *datadir) {
  pip_db_t *ret = g_new0(pip_db_t, 1);

  ret->wiredb = get_wiredb(datadir);
  if (!ret->wiredb) {
    g_free(ret);
    return NULL;
  }
  ret->memorydb = &dbrefs[0];
  return ret;
}

void
free_pipdb(pip_db_t *pipdb) {
  if (pipdb->wiredb)
    free_wiredb(pipdb->wiredb);
  g_free(pipdb);
}

/* The iterators, rewritten */
static void
__pips_of_site_append(const pip_db_t *pipdb,
		      const bitstream_parsed_t *bitstream,
		      const csite_descr_t *site,
		      GArray *pips_array) {
  const wire_db_t *wiredb = pipdb->wiredb;
  const switch_type_t sw = sw_of_type(site->type);
  const pipdb_control_t *memorydb = &pipdb->memorydb[sw];

  const pip_control_t *head = memorydb->pipctrl;
  const pip_control_t *head_end = head + memorydb->pipctrl_len;

  const uint32_t *ctrldata_array = memorydb->pipctrldata;

  if (!memorydb)
    return;

  for (; head < head_end; head++) {
    const uint32_t bitdata = query_bitstream_site_bits(bitstream, site, &ctrldata_array[head->ctrloffset], head->ctrlsize);
    const wire_atom_t endwire = head->endwire;
    const char *end = wire_name(wiredb,endwire);

    /* prepare the next iterator */
    const pip_data_t *cfgdata_array = &memorydb->pipdatadata[head->dataoffset];
    unsigned spend = head->datasize;
    unsigned sp;

    if (bitdata == 0)
      continue;

    for (sp = 0; sp < spend; sp++) {
      const pip_data_t *cfgdata_descr = cfgdata_array + sp;
      const uint32_t cfgdata = cfgdata_descr->cfgdata;

      if ( (cfgdata & bitdata) == cfgdata ) {
	const wire_atom_t startwire = cfgdata_descr->startwire;
	pip_t pip = { .source = startwire, .target = endwire };
	g_array_append_val(pips_array, pip);

	if (cfgdata != bitdata) {
	  const char *start = wire_name(wiredb, startwire);
	  debit_log(L_PIPS, "Spurious bits for %s -> %s, config %i != bitdata %i",
		    start,end,cfgdata,bitdata);
	  (void) start;
	}

	break;
      }

    } /* for sp */
    (void) end;

  } /* for head */
  return;
}

#else /* __COMPILED_PIPSDB */

#define STRINGCHUNK_DEFAULT_SIZE 16

/*
 * Forward declarations
 */

static int build_datatree_from_keyfiles(GKeyFile *data, GKeyFile *control,
					wire_db_t *wires, GNode *head,
					const site_type_t type);
static int build_connextree_from_keyfiles(GKeyFile *data, GKeyFile *control,
                                          wire_db_t *wires, GNode *head,
                                          const site_type_t type);
static void destroy_datatree(GNode *head);

#if defined(VIRTEX2)

static const gchar *basedbnames[NR_SWITCH_TYPE] = {
  [SW_CLB] = "clb",
  [SW_TTERM] = "tterm",
  [SW_BTERM] = "bterm",
  [SW_RTERM] = "rterm",
  [SW_LTERM] = "lterm",
  [SW_BIOI] = "tbioi",
  [SW_TIOI] = "tbioi",
  [SW_RIOI] = "lrioi",
  [SW_LIOI] = "lrioi",
  [SW_BRAM] = "bram",
  [SW_BTERMBRAM] = "btermbram",
  [SW_TTERMBRAM] = "ttermbram",
  [SW_BIOIBRAM] = "tbioibram",
  [SW_TIOIBRAM] = "tbioibram",
/*
  [SW_CLKT] = "clkt",
  [SW_CLKB] = "clkb",
*/
};

#elif defined(SPARTAN3)

static const gchar *basedbnames[NR_SWITCH_TYPE] = {
  [SW_CLB] = "clb",
};

#elif defined(VIRTEX4)

static const gchar *basedbnames[NR_SWITCH_TYPE] = {
  [SW_INT] = "int",
/*   [IOB] = "iob", */
/*   [CLB] = "clb", */
/*   [DSP48] = "dsp48", */
/*   [GCLKC] = "gclk", */
/*   [BRAM] = "bram", */
};

#elif defined(VIRTEX5)

static const gchar *basedbnames[NR_SWITCH_TYPE] = {
  [SW_INT] = "int",
/*   [IOB] = "iob", */
/*   [CLB] = "clb", */
/*   [DSP48] = "dsp48", */
/*   [GCLKC] = "gclk", */
/*   [BRAM] = "bram", */
};

#endif

typedef int (* build_db_t)(GKeyFile *data, GKeyFile *control,
                           wire_db_t *wires, GNode *head,
                           const site_type_t i);

static inline int
read_db(pip_db_t *pipdb,
        GNode *head, const site_type_t switchtype,
        const gchar *datadir, const gchar *base,
        const gchar *ctrlname, const gchar *dataname,
        build_db_t buildb) {
  int err = 0;
  gchar *filename = NULL;
  GKeyFile *control = NULL, *data = NULL;

  filename = g_build_filename(datadir,CHIP,base,ctrlname,NULL);
  err = read_keyfile(&control,filename);
  g_free(filename);
  if (err)
    goto out_err_free;

  filename = g_build_filename(datadir,CHIP,base,dataname,NULL);
  err = read_keyfile(&data,filename);
  g_free(filename);
  if (err)
    goto out_err_free;

  err = buildb(data, control, pipdb->wiredb, head, switchtype);

 out_err_free:
  if (control)
    g_key_file_free(control);
  if (data)
    g_key_file_free(data);
  return err;
}

static int
read_switchdb(pip_db_t *pipdb,
              GNode *head, const site_type_t switchtype,
              const gchar *datadir, const gchar *base,
              const gchar *ctrlname, const gchar *dataname) {
  return read_db(pipdb, head, switchtype,
                 datadir, base, ctrlname, dataname,
                 build_datatree_from_keyfiles);
}

static int
read_connexdb(pip_db_t *pipdb,
	      GNode *head, const site_type_t switchtype,
	      const gchar *datadir, const gchar *base,
	      const gchar *ctrlname, const gchar *dataname) {
  return read_db(pipdb, head, switchtype,
                 datadir, base, ctrlname, dataname,
                 build_connextree_from_keyfiles);
}

static GNode *
read_implicitdb(wire_db_t *wiredb, const gchar *datadir,
		const gchar *base, const gchar *dataname);

/** \brief Read a database from files
 *
 * Readback a set of files describing a pip database and fills in the
 * pip_db_t structure, in-code view of the database.
 *
 * @param pipdb the already-alloced pipdb structure to be filled in
 * @param control the name of the control db file to read
 * @param data the name of the data db file to read
 *
 * @return the status of the operation
 * @see pip_db_t
 */

#include <string.h>
static int
read_db_from_file(pip_db_t *pipdb, const gchar *datadir) {
  int err = 0;
  GNode *dbnode;
  guint i;

  for (i = 0; i < NR_SWITCH_TYPE; i++) {
    const gchar *base = basedbnames[i];

    if (!base)
      continue;

    pipdb->implicitdb[i] = read_implicitdb(pipdb->wiredb, datadir, base, "implicit.db");

    dbnode = g_node_new(NULL);
    err = read_switchdb(pipdb, dbnode, i, datadir, base,
			"control.db", "data.db");
    if (err)
      goto out_err;
    pipdb->memorydb[i] = dbnode;

    dbnode = g_node_new(NULL);
    err = read_connexdb(pipdb, dbnode, i, datadir, base,
                        "connexcontrol.db", "connexdata.db");
    if (err)
      g_warning("No connexion database for %s", base);
    pipdb->connexdb[i] = dbnode;
    /* Discard error for connexity database for now */
    err = 0;
  }

  return err;

 out_err:
  free_pipdb (pipdb);
  return err;
}

/** \brief Allocate and fill a pip database
 *
 * Load a new database into memory
 */
pip_db_t *
get_pipdb(const gchar *datadir) {
  pip_db_t *ret;

  ret = g_new0(pip_db_t, 1);

  /* XXX This to be passed as argument in final version of API */
  ret->wiredb = get_wiredb(datadir);
  if (!ret->wiredb) {
    g_free(ret);
    return NULL;
  }

  if (read_db_from_file(ret,datadir))
    return NULL;

  return ret;
}

static inline
void free_datadb(GNode **db) {
  GNode *tree = *db;
  if (tree)
    destroy_datatree(tree);
  *db = NULL;
}

static void free_impldb(GNode **db);
/** \brief Free a filled pip database
 *
 * Free all structures allocated during the database loading
 *
 * @param pipdb the filled in pipdb structure to be freed
 * @see read_db_from_file
 * @see pip_db_t
 */

void
free_pipdb(pip_db_t *pipdb) {
  guint i;

  /* XXX This to be done elsewhere final version of API */
  if (pipdb->wiredb)
    free_wiredb(pipdb->wiredb);

  for(i = 0; i < NR_SWITCH_TYPE; i++) {
    free_impldb (&pipdb->implicitdb[i]);
    free_datadb (&pipdb->memorydb[i]);
    free_datadb (&pipdb->connexdb[i]);
  }

  g_free(pipdb);
}

/** \brief Iterator over endpoint nodes in memory db
 */

static inline void
iterate_over_groups_memory(GNode *head,
			   GNodeForeachFunc func,
			   gpointer data) {
  g_node_children_foreach(head,G_TRAVERSE_ALL,func,data);
}

typedef void (*pip_hook_t)(gpointer, const gchar *, const gchar *);

static void
iterate_over_starts(GKeyFile *db, pip_hook_t func, gpointer closure,
		    const gchar *endpoint)
{
  gchar **starts;
  gsize nstarts, j;

  starts = g_key_file_get_keys(db, endpoint, &nstarts, NULL);
  if (!starts) {
    g_error("Error getting keys from db for endpoint %s",endpoint);
    return;
  }

  for (j = 0; j < nstarts; j++) {
    const gchar *start = starts[j];
    /* we could need continuations there */
    func(closure,start,endpoint);
  }

  g_strfreev(starts);
  return;
}

typedef struct _pip_iterator {
  pip_hook_t pip_iterator;
  gpointer closure_iterator;
} pip_iterator_t;

/* Serialized version of the function */

static void
iterate_over_starts_hook(GKeyFile *datadb, const gchar *endpoint, gpointer data)
{
  pip_iterator_t *iter = data;

  iterate_over_starts(datadb,
		      iter->pip_iterator,
		      iter->closure_iterator,
		      endpoint);
}

/** \brief Iterator over pips.
 *
 * This function iterates a function over all pips in the database; it
 * uses the iterator over group above
 *
 * @param pipdb the pip database to iterate over
 * @param func the function to iterate on each pip
 * @param closure the closure argument of the iterating function
 * @see pip_hook_t
 */

static void
iterate_over_pips(GKeyFile *db,
		  pip_hook_t func,
		  gpointer closure) {
  pip_iterator_t localclosure;
  localclosure.pip_iterator = func;
  localclosure.closure_iterator = closure;
  iterate_over_groups(db, iterate_over_starts_hook, &localclosure);
}

/**
 * Helper functions for building the in-memory representation of the
 * file data.
 */

typedef struct _localpip_data {
  wire_atom_t startwire;
  guint32 cfgdata;
} localpip_data_t;

typedef struct _build_wirenode {
  GKeyFile *datadb;
  GNode *groupnode;
  wire_db_t *wires;
} build_wirenode_t;

static guint32 get_pip_data_from_file(GKeyFile *keyfile,
				      const gchar *start,
				      const gchar *end);

static void
build_wirenode(gpointer data, const gchar *start, const gchar *end) {
  build_wirenode_t *exam = data;
  localpip_data_t *dat = g_new(localpip_data_t, 1);
  GNode *wirenode;

  if (parse_wire_simple(exam->wires, &dat->startwire, start)) {
    g_warning("unparsable wire %s", start);
    g_free(dat);
    return;
  }
  dat->cfgdata = get_pip_data_from_file(exam->datadb, start, end);

  wirenode = g_node_new(dat);
  g_node_append(exam->groupnode, wirenode);
}

typedef struct _localpip_control_data {
  wire_atom_t endwire;
  gsize size;
  guint32 *data;
} localpip_control_data_t;

typedef struct _build_groupnode {
  GKeyFile *ctrldb;
  wire_db_t *wires;
  GNode *head;
  const site_type_t type;
} build_groupnode_t;

/** \brief Raw query of the file representing the pip control database
 *
 * This function returns the site-independent control information
 * present in the database and directly used to locate the configuration
 * bits of the pip in the bitstream.
 *
 * @param pipdb pip database
 * @param end pip endpoint
 * @param length pointer to variable holding the number of bits part of
 * the returned configuration
 *
 * @return the list of bits
 *
 */

/* XXX Do the transform at database-generation time */

static inline gint *get_pip_structure_from_file(GKeyFile *keyfile,
						const gchar *end,
						gsize *length, const site_type_t type) {
  gint *array = g_key_file_get_integer_list(keyfile, end, "BITLIST", length, NULL);
  const guint width = type_bits[type].y_width;
  gsize i;

#if DEBIT_DEBUG > 1
  if (array == NULL) {
    g_warning("Problem parsing group %s in pip database", end);
    return NULL;
  }
#else
  g_assert(array != NULL);
#endif

  for (i = 0; i < *length; i++)
    array[i] = bitpos_to_cfgbit(array[i], width);
  return array;
}

static void build_groupnode(GKeyFile *datadb, const gchar* endp,
			    gpointer data) {
  build_groupnode_t *exam = data;
  wire_db_t *wires = exam->wires;
  localpip_control_data_t *dat = g_new(localpip_control_data_t, 1);
  GNode *groupnode;
  build_wirenode_t arg = { .datadb = datadb, .wires = wires };

  if (parse_wire_simple(wires, &dat->endwire, endp)) {
    g_warning("unparsable wire %s", endp);
    g_free(dat);
    return;
  }

  dat->data = (guint *)get_pip_structure_from_file(exam->ctrldb, endp, &dat->size, exam->type);

  groupnode = g_node_new(dat);
  g_node_append(exam->head, groupnode);
  arg.groupnode = groupnode;

  iterate_over_starts(datadb, build_wirenode, &arg, endp);
}

typedef struct _connex_control_data {
  wire_atom_t endwire;
  gsize size;
  logic_atom_t *data;
} connex_control_data_t;

/** \brief Raw query of the file representing the connexity control database
 *
 * This function returns the site-independent connexity control information
 * present in the database and directly used to locate the entry points
 * of a logic element in the design.
 *
 * @param pipdb pip database
 * @param end pip endpoint
 * @param length pointer to variable holding the number of bits part of
 * the returned configuration
 *
 * @return the list of bits
 *
 */

static inline wire_atom_t *
get_con_structure_from_file(GKeyFile *keyfile, const gchar *end, gsize *length,
			    const wire_db_t *wires) {
  gchar **endp = g_key_file_get_string_list(keyfile, end, "EPLIST", length, NULL);
  wire_atom_t *array = NULL;
  gsize i;

#if DEBIT_DEBUG > 1
  if (endp == NULL) {
    g_warning("Problem parsing group %s in connexity database", end);
    return NULL;
  }
#else
  g_assert(endp != NULL);
#endif

  array = g_new(logic_atom_t, *length);
  for (i = 0; i < *length; i++) {
    int err = parse_logic_simple(wires, &array[i], endp[i]);
    if (err) {
      g_warning("unparsable target wire \"%s\"", endp[i]);
      g_free(array);
      array = NULL;
      break;
    }
  }

  g_strfreev(endp);
  return array;
}

static void
build_connexnode(GKeyFile *datadb, const gchar* endp, gpointer data) {
  build_groupnode_t *exam = data;
  wire_db_t *wires = exam->wires;
  connex_control_data_t *dat = g_new(connex_control_data_t, 1);
  GNode *groupnode;
  build_wirenode_t arg = { .datadb = datadb, .wires = wires };

  if (parse_wire_simple(wires, &dat->endwire, endp)) {
    g_warning("unparsable wire %s", endp);
    g_free(dat);
    return;
  }

  dat->data = get_con_structure_from_file(exam->ctrldb, endp, &dat->size,
                                          exam->wires);

  if (dat->data == NULL) {
    g_warning("Not registering connexnode for %s", endp);
    g_free(dat);
    return;
  }

  g_warning("Registering connexnode for %s", endp);

  groupnode = g_node_new(dat);
  g_node_append(exam->head, groupnode);
  arg.groupnode = groupnode;

  iterate_over_starts(datadb, build_wirenode, &arg, endp);
}

/** \brief Convert ini file data into optimized in-memory representation
 *
 *
 *
 */
static int
build_datatree_from_keyfiles(GKeyFile *data, GKeyFile *control,
			     wire_db_t *wires, GNode *head,
			     const site_type_t i) {
  build_groupnode_t arg = {
    .ctrldb = control,
    .head = head,
    .wires = wires,
    .type = i,
  };

  iterate_over_groups(data, build_groupnode, &arg);
  return 0;
}

/** \brief Convert ini file data into optimized in-memory representation
 *
 *
 *
 */
static int
build_connextree_from_keyfiles(GKeyFile *data, GKeyFile *control,
                               wire_db_t *wires, GNode *head,
                               const site_type_t i) {
  build_groupnode_t arg = {
    .ctrldb = control,
    .head = head,
    .wires = wires,
    .type = i,
  };

  iterate_over_groups(data, build_connexnode, &arg);
  return 0;
}

/** \brief Release memory database
 *
 *
 */

static void release_groupnode (GNode *node,
			       gpointer data) {
  localpip_control_data_t *dat = node->data;
  (void) data;
  g_free(dat->data);
  dat->data = NULL;
}

static gboolean release_node (GNode *node,
			      gpointer data) {
  void *dat = node->data;
  (void) data;
  if (dat)
    g_free(dat);
  node->data = NULL;
  return FALSE;
}

static void
destroy_datatree(GNode *head) {
  iterate_over_groups_memory(head, release_groupnode, NULL);
  g_node_traverse(head, G_IN_ORDER, G_TRAVERSE_ALL, -1, release_node, NULL);
  g_node_destroy(head);
  return;
}

/*
 * Democode, print the DB
 */

static void
print_pip_hook(gpointer data, const gchar *start, const gchar *end) {
  (void) data;
  g_print("pip %s -> %s", start, end);
}

/** \brief Print pip database.
 *
 * This simple function uses the iterator mechanism to print the whole db.
 *
 * @param pipdb the pip database to print
 */
__attribute__((unused)) static void print_pipdb(GKeyFile *pipdb)
{
  iterate_over_pips(pipdb, print_pip_hook, NULL);
}

/** \brief Raw query of the file containing the pip data database
 *
 * This function returns the site-independent database used to identify
 * a pip in the bitstream.
 *
 * @param pipdb pip database
 * @param start pip startpoint
 * @param end pip endpoint
 *
 * @return the characteristic value of the endpoint data for the pip
 */
static guint32 get_pip_data_from_file(GKeyFile *keyfile,
				      const gchar *start,
				      const gchar *end) {
  return g_key_file_get_integer(keyfile, end, start, NULL);
}

typedef struct _examine_data_memory {
  guint32 bitstream_data;
  /* at some point get rid of wiredb and endwire */
  wire_db_t *wiredb;
  wire_atom_t endwire;
  wire_atom_t startwire;
  gboolean found;
} examine_data_memory_t;

static void examine_node_memory (GNode *node,
				 gpointer data) {
  examine_data_memory_t *exam_arg = data;
  localpip_data_t *cfgarg = node->data;
  wire_db_t *wiredb = exam_arg->wiredb;
  const gchar *end = wire_name(wiredb,exam_arg->endwire);
  const gchar *start = wire_name(wiredb,cfgarg->startwire);
  const guint32 cfgdata = cfgarg->cfgdata;
  const guint32 bitdata = exam_arg->bitstream_data;

  if ( (cfgdata & bitdata) == cfgdata ) {
    if (cfgdata != bitdata)
      debit_log(L_PIPS, "Spurious bits for %s -> %s, config %i != bitdata %i",
		start,end,cfgdata,bitdata);
    if (exam_arg->found) {
      debit_log(L_PIPS, "Not replacing %s -> %s",
		wire_name(wiredb,exam_arg->startwire),end);
    }
    else {
      exam_arg->found = TRUE;
      exam_arg->startwire = cfgarg->startwire;
    }
  }
  (void) start;
  (void) end;
}

typedef struct _examine_endpoint_memory {
  const bitstream_parsed_t *bitstream;
  const csite_descr_t *site;
  /* at some point get rid of wiredb */
  wire_db_t *wiredb;
  GArray *array;
} examine_endpoint_memory_t;

static void
examine_groupnode (GNode *node, gpointer data) {
  localpip_control_data_t *ctrldat = node->data;
  const examine_endpoint_memory_t *exam_arg = data;
  wire_db_t * wiredb = exam_arg->wiredb;
  const wire_atom_t endwire = ctrldat->endwire;
  examine_data_memory_t pass_arg = {
    .endwire = endwire,
    .found = FALSE,
    .wiredb = wiredb,
  };
  guint32 bitdata;

  /* query the bitstream about the endpoint */
  bitdata = query_bitstream_site_bits(exam_arg->bitstream, exam_arg->site,
				      ctrldat->data, ctrldat->size);

  if (bitdata == 0)
    return;

  pass_arg.bitstream_data = bitdata;

  /* iterate over pips to find out who is okay. This could just be an array. */
  /* XXX use g_node_traverse (node, G_POST_ORDER, G_TRAVERSE_LEAVES, 2,)
     to slightly speedup the process.
  */
  g_node_children_foreach(node,G_TRAVERSE_ALL,examine_node_memory,&pass_arg);

  if (pass_arg.found) {
    pip_t pip = { .source = pass_arg.startwire, .target = endwire };
    GArray *array = exam_arg->array;
    g_array_append_val(array, pip);
  }
}

static void
__pips_of_site_append(const pip_db_t *pipdb,
		      const bitstream_parsed_t *bitstream,
		      const csite_descr_t *site,
		      GArray *pips_array) {
  switch_type_t swbox = sw_of_type(site->type);
  GNode *head = pipdb->memorydb[swbox];
  const examine_endpoint_memory_t exam_arg = {
    .bitstream = bitstream,
    .site = site,
    .array = pips_array,
    .wiredb = pipdb->wiredb,
  };
  void *arg = (void *)&exam_arg;

  if (!head)
    return;

  iterate_over_groups_memory(head, examine_groupnode, arg);
}

/***
 * Implicit pip database
 */

/**
 * Build one pip node
 *
 */

typedef struct _build_pipnode {
  GNode *head;
  wire_db_t *wires;
} build_pipnode_t;

static void build_implpip(GKeyFile *datadb, const gchar* endp,
			  gpointer data) {
  build_pipnode_t *exam = data;
  wire_db_t *wires = exam->wires;
  pip_t *pip = g_new(pip_t, 1);
  GError *error = NULL;
  GNode *groupnode;
  gchar *sourcename;

  if (parse_wire_simple(wires, &pip->target, endp)) {
    g_warning("unparsable target wire \"%s\"", endp);
    goto out_err;
  }

  sourcename = g_key_file_get_string(datadb, endp, "EP", &error);
  if (error)
    goto out_free_err;

  if (parse_wire_simple(wires, &pip->source, sourcename)) {
    g_warning("unparsable source wire \"%s\"", sourcename);
    g_free(sourcename);
    goto out_err;
  }

  g_free(sourcename);
  groupnode = g_node_new(pip);
  g_node_append(exam->head, groupnode);
  return;

 out_free_err:
  g_error_free(error);
 out_err:
  g_free(pip);
  return;
}

/** \brief Prepare the iteration over a keyfile describing an implicit
 * pip database
 *
 */

static inline GNode *
build_impldb_from_keyfile(GKeyFile *impldb, wire_db_t *wires) {
  build_pipnode_t arg = {
    .head = g_node_new(NULL),
    .wires = wires,
  };

  iterate_over_groups(impldb, build_implpip, &arg);
  return arg.head;
}

/** \brief Read an implicit pip database
 *
 * Readback a file containing implicit pips present at a site.
 *
 */
static GNode *
read_implicitdb(wire_db_t *wiredb,
		const gchar *datadir, const gchar *base,
		const gchar *dataname) {
  GNode *head = NULL;
  gchar *filename = NULL;
  GKeyFile *data = NULL;
  int err;

  filename = g_build_filename(datadir,CHIP,base,dataname,NULL);
  err = read_keyfile(&data,filename);
  g_free(filename);
  if (err)
    goto out_err_free;

  head = build_impldb_from_keyfile(data, wiredb);

 out_err_free:
  if (data)
    g_key_file_free(data);
  return head;
}

static void
destroy_implicit(GNode *head) {
  g_node_traverse(head, G_IN_ORDER, G_TRAVERSE_ALL, -1, release_node, NULL);
  g_node_destroy(head);
  return;
}

/** \brief Free an implicit pip database
 *
 */

static
void free_impldb(GNode **db) {
  GNode *tree = *db;
  if (tree)
    destroy_implicit(tree);
  *db = NULL;
}

static gboolean
idpips(GNode *node, gpointer data) {
  const pip_t *tpip = node->data;
  pip_t *npip = data;
  if (tpip->target == npip->target) {
    npip->source = tpip->source;
    return TRUE;
  }
  return FALSE;
}

/** \brief Query an implicit pip database
 *
 */

static gboolean
query_impldb(GNode *db, wire_atom_t *wire,
	     const wire_atom_t orig) {
  pip_t pip = { .source = WIRE_EP_END, .target = orig };
  g_node_traverse(db, G_IN_ORDER, G_TRAVERSE_LEAVES,
		  2, idpips, &pip);
  if (pip.source == WIRE_EP_END)
    return FALSE;

  *wire = pip.source;
  return TRUE;
}

static gboolean
_get_implicit_startpoint(wire_atom_t *wire,
			 const pip_db_t *pipdb,
			 const wire_atom_t orig,
			 const site_type_t stype) {
  GNode *db = pipdb->implicitdb[stype];

  if (db)
    return query_impldb(db, wire, orig);
  return FALSE;
}

#endif /* __COMPILED_PIPSDB */

/** \brief Query a bitstream for the pips contained in a site, in-memory version
 *
 * This is a very raw unoptimized version which should be must faster already
 *
 * @param pipdb the pip database
 * @param bitstream the bitstream data
 * @param site the site queried
 * @param size pointer to return the size of the db
 *
 * @return the list of pips which are present at the location
 * @return the length of this pip list
 */
static pip_t *
__pips_of_site_memory(const pip_db_t *pipdb,
		      const bitstream_parsed_t *bitstream,
		      const csite_descr_t *site,
		      gsize *size) {
  GArray *pips_array = g_array_new(FALSE, FALSE, sizeof(pip_t));
  __pips_of_site_append(pipdb, bitstream, site, pips_array);
  *size = pips_array->len;
  return (pip_t *) g_array_free (pips_array, FALSE);
}

pip_t *pips_of_site(const pip_db_t *pipdb,
		    const bitstream_parsed_t *bitstream,
		    const csite_descr_t *site,
		    gsize *size) {
  return __pips_of_site_memory(pipdb, bitstream, site, size);
}

/** \brief Do the bitstream pip interpretation in an ad-hoc, fast lookup
 * structure. This is the read/write mode, which is vaguely unoptimized,
 * but will allow for read-write operation. This is the filling
 * function, which takes an already-allocated structure.
 *
 * @param pipdb the pip database
 * @param chipdb the chip description
 * @param bitstream the bitstream data to read from
 *
 * @return a pip_parsed_t structure containing the bitstream structure
 */

typedef struct _allpips_iter {
  const bitstream_parsed_t *bitstream;
  const pip_db_t *pipdb;
  unsigned site_idx;
  unsigned *site_index;
  GArray *array;
} allpips_iter_t;

static void
__pips_of_site_append_index(const pip_db_t *pipdb,
			    const bitstream_parsed_t *bitstream,
			    const csite_descr_t *site,
			    allpips_iter_t *data) {
  unsigned *site_index_a = data->site_index;
  GArray *pips_array = data->array;

  site_index_a[data->site_idx++] = pips_array->len;
  __pips_of_site_append(pipdb, bitstream, site, pips_array);
}

static void
_pips_of_bitstream_iter(unsigned site_x, unsigned site_y,
			csite_descr_t *site, gpointer dat) {
  allpips_iter_t *data = dat;

  (void) site_x;
  (void) site_y;

  /* Get back our pips */
  __pips_of_site_append_index(data->pipdb, data->bitstream, site, data);
}

static int
_pips_of_bitstream(const pip_db_t *pipdb, const chip_descr_t *chipdb,
		   const bitstream_parsed_t *bitstream,
		   pip_parsed_dense_t *fill) {
  /* This array will hold *all* of the pips */
  GArray *pips_array = g_array_new(FALSE, FALSE, sizeof(pip_t));
  gsize nsites = chipdb->width * chipdb->height;
  /* XXX This is obviously we bigger than needed. We need to redo this
     part */
  unsigned array_len = nsites;
  unsigned *site_index_a = g_new0(unsigned, array_len + 1);

  allpips_iter_t arg = {
    .bitstream = bitstream,
    .pipdb = pipdb,
    .array = pips_array,
    .site_idx = 0,
    .site_index = site_index_a,
  };

  iterate_over_sites(chipdb, _pips_of_bitstream_iter, &arg);

  site_index_a[array_len] = pips_array->len;
  debit_log(L_PIPS, "Got %i explicit pips", pips_array->len);

  fill->site_index = site_index_a;
  fill->bitpips = (pip_t *)g_array_free (pips_array, FALSE);

  return 0;
}

pip_parsed_dense_t *
pips_of_bitstream(const pip_db_t *pipdb, const chip_descr_t *chipdb,
		  const bitstream_parsed_t *bitstream) {
  pip_parsed_dense_t *dense = g_new(pip_parsed_dense_t, 1);
  int err;
  err = _pips_of_bitstream(pipdb, chipdb, bitstream, dense);
  if (err) {
    g_free(dense);
    return NULL;
  }
  return dense;
}

void free_pipdat(pip_parsed_dense_t *pipdat) {
  g_free(pipdat->site_index);
  g_free(pipdat->bitpips);
  g_free(pipdat);
}

/** \brief Query the pip database to get the origin of a pip
 *
 * This function guarantees that the spip won't be touched if
 * it returns FALSE. Other parts of the code rely on this !
 *
 * @param pipdb the pip database
 * @param wire the wire to fill in
 * @param orig the source wire
 *
 * @return if there's a pip driving the origin at the given site
 */

gboolean
get_interconnect_startpoint(const pip_parsed_dense_t *pipdat,
			    wire_atom_t *wire,
			    const wire_atom_t orig,
			    const site_ref_t site) {
  unsigned stidx = site_index(site);
  unsigned *indexes = pipdat->site_index;
  unsigned start = indexes[stidx], end = indexes[stidx+1];

  /* Do a run over the set of points for a site */
  while (start < end) {
    pip_t *pip = &pipdat->bitpips[start++];
    if (pip->target == orig) {
      *wire = pip->source;
      return TRUE;
    }
  }
  return FALSE;
}

pip_t *
pips_of_site_dense(const pip_parsed_dense_t *pipdat,
		   const site_ref_t site,
		   gsize *size) {
  unsigned stidx = site_index(site);
  unsigned *indexes = pipdat->site_index;
  unsigned start = indexes[stidx], end = indexes[stidx+1];

  *size = (end - start);
  return &pipdat->bitpips[start];
}

/** \brief Iterator over pips which are set in the bitstream
 */

void
iterate_over_bitpips(const pip_parsed_dense_t *pipdat,
		     const chip_descr_t *chip,
		     bitpip_iterator_t fun, gpointer data) {
  unsigned nsites = chip->width * chip->height;
  site_ref_t site = 0;
  unsigned *indexes = pipdat->site_index;
  pip_t *bitpips = pipdat->bitpips;
  unsigned start = 0;

  for (site = 0; site < nsites; site++) {
      unsigned end = indexes[site+1];
      for ( ; start < end; start++) {
	debit_log(L_PIPS, "calling iterator for site #%i", site);
	fun(data, bitpips[start], site);
      }
  }
}

/*
 * Complex iterator needed for optimal display.
 * A function is called on site change if the site has some pips.
 * The result of the function is used to skip -- or not -- the
 * pip iteration (result is FALSE) or not (when result is TRUE).
 *
 * For pips, for instance, this can be used to compute only once
 * the site string.
 *
 */

void
iterate_over_bitpips_complex(const pip_parsed_dense_t *pipdat,
			     const chip_descr_t *chip,
			     sitepip_iterator_t fun1, bitpip_iterator_t fun2,
			     gpointer data) {
  const unsigned width = chip->width;
  const unsigned nsites = width * chip->height;
  site_ref_t site = 0;
  unsigned *indexes = pipdat->site_index;
  pip_t *bitpips = pipdat->bitpips;
  unsigned start = 0;

  for (site = 0; site < nsites; site++) {
    unsigned end = indexes[site+1];
    csite_descr_t *csite = get_site(chip, site);

    if (!fun1(site % width, site / width, csite, data))
      start = end;

    for ( ; start < end; start++) {
      debit_log(L_PIPS, "calling iterator for site #%i", site);
      fun2(data, bitpips[start], site);
    }
  }
}

#ifndef __COMPILED_PIPSDB

/*
 * Implicit database implementation
 */

gboolean
get_implicit_startpoint(wire_atom_t *wire,
			const pip_db_t *pipdb,
			const chip_descr_t *chip,
			const wire_atom_t orig,
			const site_ref_t site) {
  site_type_t stype = site_type(chip,site);
  return _get_implicit_startpoint(wire, pipdb, orig, stype);
}

/*
 * Logic database implementation
 *
 * The logic database implementation is a three-level lookup table:
 *
 * First level, the wire which is driven (as in pips) and type of the
 * pip (LUTs for instance need a special function to be called).  Along
 * with the wire, the control bits for the programmable part.
 *
 * Second level, a bitpattern / value pair, which links the bit pattern
 * to the XDL value of the configuration. Some pips have a "default"
 * behaviour, which is a bit confusing, because the same bit pattern can
 * yield two different values in the XDL file.
 * - the bit pattern in question is usually ZERO (we assume this)
 * - the configuration option can be #OFF or its default value in the
 * XDL file. The safest way should be to set it to its default
 * value. However, we may want to do things properly and use connexity
 * analysis to put the correct XDL value in place. The active value at
 * default level of the pip should be recorded in a fixed place for fast
 * lookup.
 *
 * The third level, if type of the first level indicates that it is
 * needed, is a bitpattern / input wires needed pair. This indicates
 * which wires in the connexity analysis must be followed through, ie
 * the FANIN at the site. This is, for now, not very well tested, and
 * will be further explored when the code for this is written in
 * connexity analysis.
 *
 */

/*
 * Logic database implementation
 * Logic patterns are completely merged into the standard database _AND_
 * the implicit database. The difference between the logic and wiring
 * lies in the connexity analysis: logic wires are registered some
 * strange places. And the connexity is complex, needs a connexity
 * database, which is a bitmask of input wires.
 */

/* Generic database query functions */

typedef struct _search_db {
  const unsigned first;
  const unsigned second;
  GNode *db;
} search_db_t;

static void
find_first(GNode *node, gpointer data) {
  search_db_t *cfg = data;
  /* compatible with localpip_control_data_t for endwire */
  connex_control_data_t *dat = node->data;
  if (dat->endwire == cfg->first)
    cfg->db = node;
}

static void
find_second(GNode *node, gpointer data) {
  search_db_t *cfg = data;
  localpip_data_t *dat = node->data;
  if (dat->startwire == cfg->second)
    cfg->db = node;
}

/*
 * Return the GNode associated to a logic pip.
 * And Nil if not found.
 */

static const GNode *
db_lookup(const unsigned source,
	  const unsigned target,
	  const GNode *database) {
  search_db_t logic = { .first = target,
			.second = source,
			.db = NULL };
  if (!database)
    return NULL;

  /* First-level lookup */
  g_node_children_foreach ((void *)database, G_TRAVERSE_ALL,
                           find_first, &logic);

  if (!logic.db)
    return NULL;
  database = logic.db;
  logic.db = NULL;

  /* Second-level lookup */
  g_node_children_foreach ((void *)database, G_TRAVERSE_ALL,
                           find_second, &logic);

  return logic.db;
}

/*
 * This function gets the input wires from a logic_t entity. More
 * precisely, it is a very low-level function that returns two things:
 *
 * - the array of input wires for the pip
 * - the value of the pip's indexes
 */

/* Return from the GNode */
static uint32_t
get_input_wires_helper(const GNode *db,
                       gsize *size, logic_atom_t **array) {
  /* Get the control information */
  localpip_data_t *dat = db->data;
  GNode *ctrl = db->parent;
  connex_control_data_t *cfg = ctrl->data;
  *array = cfg->data;
  *size = cfg->size;
  return dat->cfgdata;
}

uint32_t
get_input_wires(const pip_db_t *pipdb,
                const logic_t pip, const switch_type_t swit,
                gsize *size, wire_atom_t **array) {
  GNode *database = pipdb->connexdb[swit];
  const GNode *base = db_lookup(pip.source, pip.target, database);
  return get_input_wires_helper(base, size, array);
}

static inline void
iterate_input_wires(uint32_t set, gsize size, logic_atom_t *array,
                    logic_callback_t logcall, void *data) {
  unsigned i;
  for (i=0; i<size; i++)
    if (set & (1 << i))
      logcall(array[i], data);
}

void
iter_logic_input(const pip_db_t *pipdb,
                 const logic_t pip, const switch_type_t swit,
                 logic_callback_t logcall, void *data) {
  gsize size; logic_atom_t *array;
  uint32_t set = get_input_wires(pipdb, pip, swit, &size, &array);
  iterate_input_wires(set, size, array, logcall, data);
}

/* Pip database query */
static inline void
gather_db_data(const GNode *base,
	       const unsigned **cfgbits, size_t *nbits,
	       uint32_t *vals) {
  const localpip_data_t *dat = base->data;
  const GNode *nodectrl = base->parent;
  const localpip_control_data_t *ctrl = nodectrl->data;
  *cfgbits = ctrl->data;
  *vals = dat->cfgdata;
  *nbits = ctrl->size;
}

/*
 * Return the GNode associated to a logic pip.
 * And Nil if not found.
 */

int
bitpip_lookup(const sited_pip_t spip,
	      const chip_descr_t *chip,
	      const pip_db_t *pipdb,
	      const unsigned **cfgbits, size_t *nbits,
	      uint32_t *vals) {
  const site_type_t stype = site_type(chip, spip.site);
  const GNode *database = pipdb->memorydb[stype];
  const GNode *base = db_lookup(spip.pip.source, spip.pip.target, database);

  if (!base) {
    debit_log(L_PIPS, "bitpip lookup failed");
    return -1;
  }

  gather_db_data(base, cfgbits, nbits, vals);
  debit_log(L_PIPS, "bitpip lookup succeeded with value %08x", *vals);
  return 0;
}

#else /* __COMPILED_PIPSDB */


gboolean
get_implicit_startpoint(wire_atom_t *wire,
			const pip_db_t *pipdb,
			const chip_descr_t *chip,
			const wire_atom_t orig,
			const site_ref_t site) {
  (void) wire; (void) pipdb; (void) chip;
  (void) orig; (void) site;
  return FALSE;
}

int
bitpip_lookup(const sited_pip_t spip,
	      const chip_descr_t *chip,
	      const pip_db_t *pipdb,
	      const unsigned **cfgbits, size_t *nbits,
	      uint32_t *vals) {
  (void) spip; (void) chip; (void) pipdb;
  (void) cfgbits; (void) nbits; (void) vals;
  return -1;
}

#endif /* __COMPILED_PIPSDB */
