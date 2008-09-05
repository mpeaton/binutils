/* Symbol table lookup for the GNU debugger, GDB.

   Copyright (C) 1986, 1987, 1988, 1989, 1990, 1991, 1992, 1993, 1994, 1995,
   1996, 1997, 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2007, 2008
   Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "symtab.h"
#include "gdbtypes.h"
#include "gdbcore.h"
#include "frame.h"
#include "target.h"
#include "value.h"
#include "symfile.h"
#include "objfiles.h"
#include "gdbcmd.h"
#include "call-cmds.h"
#include "gdb_regex.h"
#include "expression.h"
#include "language.h"
#include "demangle.h"
#include "inferior.h"
#include "linespec.h"
#include "source.h"
#include "filenames.h"		/* for FILENAME_CMP */
#include "objc-lang.h"
#include "ada-lang.h"
#include "p-lang.h"
#include "addrmap.h"

#include "hashtab.h"

#include "gdb_obstack.h"
#include "block.h"
#include "dictionary.h"

#include <sys/types.h>
#include <fcntl.h>
#include "gdb_string.h"
#include "gdb_stat.h"
#include <ctype.h>
#include "cp-abi.h"
#include "observer.h"
#include "gdb_assert.h"
#include "solist.h"

/* Prototypes for local functions */

static void completion_list_add_name (char *, char *, int, char *, char *);

static void rbreak_command (char *, int);

static void types_info (char *, int);

static void functions_info (char *, int);

static void variables_info (char *, int);

static void sources_info (char *, int);

static void output_source_filename (const char *, int *);

static int find_line_common (struct linetable *, int, int *);

/* This one is used by linespec.c */

char *operator_chars (char *p, char **end);

static struct symbol *lookup_symbol_aux (const char *name,
					 const char *linkage_name,
					 const struct block *block,
					 const domain_enum domain,
					 enum language language,
					 int *is_a_field_of_this);

static
struct symbol *lookup_symbol_aux_local (const char *name,
					const char *linkage_name,
					const struct block *block,
					const domain_enum domain);

static
struct symbol *lookup_symbol_aux_symtabs (int block_index,
					  const char *name,
					  const char *linkage_name,
					  const domain_enum domain);

static
struct symbol *lookup_symbol_aux_psymtabs (int block_index,
					   const char *name,
					   const char *linkage_name,
					   const domain_enum domain);

static int file_matches (char *, char **, int);

static void print_symbol_info (domain_enum,
			       struct symtab *, struct symbol *, int, char *);

static void print_msymbol_info (struct minimal_symbol *);

static void symtab_symbol_info (char *, domain_enum, int);

void _initialize_symtab (void);

/* */

/* Allow the user to configure the debugger behavior with respect
   to multiple-choice menus when more than one symbol matches during
   a symbol lookup.  */

const char multiple_symbols_ask[] = "ask";
const char multiple_symbols_all[] = "all";
const char multiple_symbols_cancel[] = "cancel";
static const char *multiple_symbols_modes[] =
{
  multiple_symbols_ask,
  multiple_symbols_all,
  multiple_symbols_cancel,
  NULL
};
static const char *multiple_symbols_mode = multiple_symbols_all;

/* Read-only accessor to AUTO_SELECT_MODE.  */

const char *
multiple_symbols_select_mode (void)
{
  return multiple_symbols_mode;
}

/* The single non-language-specific builtin type */
struct type *builtin_type_error;

/* Block in which the most recently searched-for symbol was found.
   Might be better to make this a parameter to lookup_symbol and 
   value_of_this. */

const struct block *block_found;

/* Check for a symtab of a specific name; first in symtabs, then in
   psymtabs.  *If* there is no '/' in the name, a match after a '/'
   in the symtab filename will also work.  */

struct symtab *
lookup_symtab (const char *name)
{
  struct symtab *s;
  struct partial_symtab *ps;
  struct objfile *objfile;
  char *real_path = NULL;
  char *full_path = NULL;

  /* Here we are interested in canonicalizing an absolute path, not
     absolutizing a relative path.  */
  if (IS_ABSOLUTE_PATH (name))
    {
      full_path = xfullpath (name);
      make_cleanup (xfree, full_path);
      real_path = gdb_realpath (name);
      make_cleanup (xfree, real_path);
    }

got_symtab:

  /* First, search for an exact match */

  ALL_SYMTABS (objfile, s)
  {
    if (FILENAME_CMP (name, s->filename) == 0)
      {
	return s;
      }
      
    /* If the user gave us an absolute path, try to find the file in
       this symtab and use its absolute path.  */
    
    if (full_path != NULL)
      {
        const char *fp = symtab_to_fullname (s);
        if (fp != NULL && FILENAME_CMP (full_path, fp) == 0)
          {
            return s;
          }
      }

    if (real_path != NULL)
      {
        char *fullname = symtab_to_fullname (s);
        if (fullname != NULL)
          {
            char *rp = gdb_realpath (fullname);
            make_cleanup (xfree, rp);
            if (FILENAME_CMP (real_path, rp) == 0)
              {
                return s;
              }
          }
      }
  }

  /* Now, search for a matching tail (only if name doesn't have any dirs) */

  if (lbasename (name) == name)
    ALL_SYMTABS (objfile, s)
    {
      if (FILENAME_CMP (lbasename (s->filename), name) == 0)
	return s;
    }

  /* Same search rules as above apply here, but now we look thru the
     psymtabs.  */

  ps = lookup_partial_symtab (name);
  if (!ps)
    return (NULL);

  if (ps->readin)
    error (_("Internal: readin %s pst for `%s' found when no symtab found."),
	   ps->filename, name);

  s = PSYMTAB_TO_SYMTAB (ps);

  if (s)
    return s;

  /* At this point, we have located the psymtab for this file, but
     the conversion to a symtab has failed.  This usually happens
     when we are looking up an include file.  In this case,
     PSYMTAB_TO_SYMTAB doesn't return a symtab, even though one has
     been created.  So, we need to run through the symtabs again in
     order to find the file.
     XXX - This is a crock, and should be fixed inside of the the
     symbol parsing routines. */
  goto got_symtab;
}

/* Lookup the partial symbol table of a source file named NAME.
   *If* there is no '/' in the name, a match after a '/'
   in the psymtab filename will also work.  */

struct partial_symtab *
lookup_partial_symtab (const char *name)
{
  struct partial_symtab *pst;
  struct objfile *objfile;
  char *full_path = NULL;
  char *real_path = NULL;

  /* Here we are interested in canonicalizing an absolute path, not
     absolutizing a relative path.  */
  if (IS_ABSOLUTE_PATH (name))
    {
      full_path = xfullpath (name);
      make_cleanup (xfree, full_path);
      real_path = gdb_realpath (name);
      make_cleanup (xfree, real_path);
    }

  ALL_PSYMTABS (objfile, pst)
  {
    if (FILENAME_CMP (name, pst->filename) == 0)
      {
	return (pst);
      }

    /* If the user gave us an absolute path, try to find the file in
       this symtab and use its absolute path.  */
    if (full_path != NULL)
      {
	psymtab_to_fullname (pst);
	if (pst->fullname != NULL
	    && FILENAME_CMP (full_path, pst->fullname) == 0)
	  {
	    return pst;
	  }
      }

    if (real_path != NULL)
      {
        char *rp = NULL;
	psymtab_to_fullname (pst);
        if (pst->fullname != NULL)
          {
            rp = gdb_realpath (pst->fullname);
            make_cleanup (xfree, rp);
          }
	if (rp != NULL && FILENAME_CMP (real_path, rp) == 0)
	  {
	    return pst;
	  }
      }
  }

  /* Now, search for a matching tail (only if name doesn't have any dirs) */

  if (lbasename (name) == name)
    ALL_PSYMTABS (objfile, pst)
    {
      if (FILENAME_CMP (lbasename (pst->filename), name) == 0)
	return (pst);
    }

  return (NULL);
}

/* Mangle a GDB method stub type.  This actually reassembles the pieces of the
   full method name, which consist of the class name (from T), the unadorned
   method name from METHOD_ID, and the signature for the specific overload,
   specified by SIGNATURE_ID.  Note that this function is g++ specific. */

char *
gdb_mangle_name (struct type *type, int method_id, int signature_id)
{
  int mangled_name_len;
  char *mangled_name;
  struct fn_field *f = TYPE_FN_FIELDLIST1 (type, method_id);
  struct fn_field *method = &f[signature_id];
  char *field_name = TYPE_FN_FIELDLIST_NAME (type, method_id);
  char *physname = TYPE_FN_FIELD_PHYSNAME (f, signature_id);
  char *newname = type_name_no_tag (type);

  /* Does the form of physname indicate that it is the full mangled name
     of a constructor (not just the args)?  */
  int is_full_physname_constructor;

  int is_constructor;
  int is_destructor = is_destructor_name (physname);
  /* Need a new type prefix.  */
  char *const_prefix = method->is_const ? "C" : "";
  char *volatile_prefix = method->is_volatile ? "V" : "";
  char buf[20];
  int len = (newname == NULL ? 0 : strlen (newname));

  /* Nothing to do if physname already contains a fully mangled v3 abi name
     or an operator name.  */
  if ((physname[0] == '_' && physname[1] == 'Z')
      || is_operator_name (field_name))
    return xstrdup (physname);

  is_full_physname_constructor = is_constructor_name (physname);

  is_constructor =
    is_full_physname_constructor || (newname && strcmp (field_name, newname) == 0);

  if (!is_destructor)
    is_destructor = (strncmp (physname, "__dt", 4) == 0);

  if (is_destructor || is_full_physname_constructor)
    {
      mangled_name = (char *) xmalloc (strlen (physname) + 1);
      strcpy (mangled_name, physname);
      return mangled_name;
    }

  if (len == 0)
    {
      sprintf (buf, "__%s%s", const_prefix, volatile_prefix);
    }
  else if (physname[0] == 't' || physname[0] == 'Q')
    {
      /* The physname for template and qualified methods already includes
         the class name.  */
      sprintf (buf, "__%s%s", const_prefix, volatile_prefix);
      newname = NULL;
      len = 0;
    }
  else
    {
      sprintf (buf, "__%s%s%d", const_prefix, volatile_prefix, len);
    }
  mangled_name_len = ((is_constructor ? 0 : strlen (field_name))
		      + strlen (buf) + len + strlen (physname) + 1);

    {
      mangled_name = (char *) xmalloc (mangled_name_len);
      if (is_constructor)
	mangled_name[0] = '\0';
      else
	strcpy (mangled_name, field_name);
    }
  strcat (mangled_name, buf);
  /* If the class doesn't have a name, i.e. newname NULL, then we just
     mangle it using 0 for the length of the class.  Thus it gets mangled
     as something starting with `::' rather than `classname::'. */
  if (newname != NULL)
    strcat (mangled_name, newname);

  strcat (mangled_name, physname);
  return (mangled_name);
}


/* Initialize the language dependent portion of a symbol
   depending upon the language for the symbol. */
void
symbol_init_language_specific (struct general_symbol_info *gsymbol,
			       enum language language)
{
  gsymbol->language = language;
  if (gsymbol->language == language_cplus
      || gsymbol->language == language_java
      || gsymbol->language == language_objc)
    {
      gsymbol->language_specific.cplus_specific.demangled_name = NULL;
    }
  else
    {
      memset (&gsymbol->language_specific, 0,
	      sizeof (gsymbol->language_specific));
    }
}

/* Functions to initialize a symbol's mangled name.  */

/* Create the hash table used for demangled names.  Each hash entry is
   a pair of strings; one for the mangled name and one for the demangled
   name.  The entry is hashed via just the mangled name.  */

static void
create_demangled_names_hash (struct objfile *objfile)
{
  /* Choose 256 as the starting size of the hash table, somewhat arbitrarily.
     The hash table code will round this up to the next prime number. 
     Choosing a much larger table size wastes memory, and saves only about
     1% in symbol reading.  */

  objfile->demangled_names_hash = htab_create_alloc
    (256, htab_hash_string, (int (*) (const void *, const void *)) streq,
     NULL, xcalloc, xfree);
}

/* Try to determine the demangled name for a symbol, based on the
   language of that symbol.  If the language is set to language_auto,
   it will attempt to find any demangling algorithm that works and
   then set the language appropriately.  The returned name is allocated
   by the demangler and should be xfree'd.  */

static char *
symbol_find_demangled_name (struct general_symbol_info *gsymbol,
			    const char *mangled)
{
  char *demangled = NULL;

  if (gsymbol->language == language_unknown)
    gsymbol->language = language_auto;

  if (gsymbol->language == language_objc
      || gsymbol->language == language_auto)
    {
      demangled =
	objc_demangle (mangled, 0);
      if (demangled != NULL)
	{
	  gsymbol->language = language_objc;
	  return demangled;
	}
    }
  if (gsymbol->language == language_cplus
      || gsymbol->language == language_auto)
    {
      demangled =
        cplus_demangle (mangled, DMGL_PARAMS | DMGL_ANSI);
      if (demangled != NULL)
	{
	  gsymbol->language = language_cplus;
	  return demangled;
	}
    }
  if (gsymbol->language == language_java)
    {
      demangled =
        cplus_demangle (mangled,
                        DMGL_PARAMS | DMGL_ANSI | DMGL_JAVA);
      if (demangled != NULL)
	{
	  gsymbol->language = language_java;
	  return demangled;
	}
    }
  return NULL;
}

/* Set both the mangled and demangled (if any) names for GSYMBOL based
   on LINKAGE_NAME and LEN.  The hash table corresponding to OBJFILE
   is used, and the memory comes from that objfile's objfile_obstack.
   LINKAGE_NAME is copied, so the pointer can be discarded after
   calling this function.  */

/* We have to be careful when dealing with Java names: when we run
   into a Java minimal symbol, we don't know it's a Java symbol, so it
   gets demangled as a C++ name.  This is unfortunate, but there's not
   much we can do about it: but when demangling partial symbols and
   regular symbols, we'd better not reuse the wrong demangled name.
   (See PR gdb/1039.)  We solve this by putting a distinctive prefix
   on Java names when storing them in the hash table.  */

/* FIXME: carlton/2003-03-13: This is an unfortunate situation.  I
   don't mind the Java prefix so much: different languages have
   different demangling requirements, so it's only natural that we
   need to keep language data around in our demangling cache.  But
   it's not good that the minimal symbol has the wrong demangled name.
   Unfortunately, I can't think of any easy solution to that
   problem.  */

#define JAVA_PREFIX "##JAVA$$"
#define JAVA_PREFIX_LEN 8

void
symbol_set_names (struct general_symbol_info *gsymbol,
		  const char *linkage_name, int len, struct objfile *objfile)
{
  char **slot;
  /* A 0-terminated copy of the linkage name.  */
  const char *linkage_name_copy;
  /* A copy of the linkage name that might have a special Java prefix
     added to it, for use when looking names up in the hash table.  */
  const char *lookup_name;
  /* The length of lookup_name.  */
  int lookup_len;

  if (objfile->demangled_names_hash == NULL)
    create_demangled_names_hash (objfile);

  if (gsymbol->language == language_ada)
    {
      /* In Ada, we do the symbol lookups using the mangled name, so
         we can save some space by not storing the demangled name.

         As a side note, we have also observed some overlap between
         the C++ mangling and Ada mangling, similarly to what has
         been observed with Java.  Because we don't store the demangled
         name with the symbol, we don't need to use the same trick
         as Java.  */
      gsymbol->name = obstack_alloc (&objfile->objfile_obstack, len + 1);
      memcpy (gsymbol->name, linkage_name, len);
      gsymbol->name[len] = '\0';
      gsymbol->language_specific.cplus_specific.demangled_name = NULL;

      return;
    }

  /* The stabs reader generally provides names that are not
     NUL-terminated; most of the other readers don't do this, so we
     can just use the given copy, unless we're in the Java case.  */
  if (gsymbol->language == language_java)
    {
      char *alloc_name;
      lookup_len = len + JAVA_PREFIX_LEN;

      alloc_name = alloca (lookup_len + 1);
      memcpy (alloc_name, JAVA_PREFIX, JAVA_PREFIX_LEN);
      memcpy (alloc_name + JAVA_PREFIX_LEN, linkage_name, len);
      alloc_name[lookup_len] = '\0';

      lookup_name = alloc_name;
      linkage_name_copy = alloc_name + JAVA_PREFIX_LEN;
    }
  else if (linkage_name[len] != '\0')
    {
      char *alloc_name;
      lookup_len = len;

      alloc_name = alloca (lookup_len + 1);
      memcpy (alloc_name, linkage_name, len);
      alloc_name[lookup_len] = '\0';

      lookup_name = alloc_name;
      linkage_name_copy = alloc_name;
    }
  else
    {
      lookup_len = len;
      lookup_name = linkage_name;
      linkage_name_copy = linkage_name;
    }

  slot = (char **) htab_find_slot (objfile->demangled_names_hash,
				   lookup_name, INSERT);

  /* If this name is not in the hash table, add it.  */
  if (*slot == NULL)
    {
      char *demangled_name = symbol_find_demangled_name (gsymbol,
							 linkage_name_copy);
      int demangled_len = demangled_name ? strlen (demangled_name) : 0;

      /* If there is a demangled name, place it right after the mangled name.
	 Otherwise, just place a second zero byte after the end of the mangled
	 name.  */
      *slot = obstack_alloc (&objfile->objfile_obstack,
			     lookup_len + demangled_len + 2);
      memcpy (*slot, lookup_name, lookup_len + 1);
      if (demangled_name != NULL)
	{
	  memcpy (*slot + lookup_len + 1, demangled_name, demangled_len + 1);
	  xfree (demangled_name);
	}
      else
	(*slot)[lookup_len + 1] = '\0';
    }

  gsymbol->name = *slot + lookup_len - len;
  if ((*slot)[lookup_len + 1] != '\0')
    gsymbol->language_specific.cplus_specific.demangled_name
      = &(*slot)[lookup_len + 1];
  else
    gsymbol->language_specific.cplus_specific.demangled_name = NULL;
}

/* Return the source code name of a symbol.  In languages where
   demangling is necessary, this is the demangled name.  */

char *
symbol_natural_name (const struct general_symbol_info *gsymbol)
{
  switch (gsymbol->language) 
    {
    case language_cplus:
    case language_java:
    case language_objc:
      if (gsymbol->language_specific.cplus_specific.demangled_name != NULL)
	return gsymbol->language_specific.cplus_specific.demangled_name;
      break;
    case language_ada:
      if (gsymbol->language_specific.cplus_specific.demangled_name != NULL)
	return gsymbol->language_specific.cplus_specific.demangled_name;
      else
	return ada_decode_symbol (gsymbol);
      break;
    default:
      break;
    }
  return gsymbol->name;
}

/* Return the demangled name for a symbol based on the language for
   that symbol.  If no demangled name exists, return NULL. */
char *
symbol_demangled_name (const struct general_symbol_info *gsymbol)
{
  switch (gsymbol->language) 
    {
    case language_cplus:
    case language_java:
    case language_objc:
      if (gsymbol->language_specific.cplus_specific.demangled_name != NULL)
	return gsymbol->language_specific.cplus_specific.demangled_name;
      break;
    case language_ada:
      if (gsymbol->language_specific.cplus_specific.demangled_name != NULL)
	return gsymbol->language_specific.cplus_specific.demangled_name;
      else
	return ada_decode_symbol (gsymbol);
      break;
    default:
      break;
    }
  return NULL;
}

/* Return the search name of a symbol---generally the demangled or
   linkage name of the symbol, depending on how it will be searched for.
   If there is no distinct demangled name, then returns the same value 
   (same pointer) as SYMBOL_LINKAGE_NAME. */
char *
symbol_search_name (const struct general_symbol_info *gsymbol)
{
  if (gsymbol->language == language_ada)
    return gsymbol->name;
  else
    return symbol_natural_name (gsymbol);
}

/* Initialize the structure fields to zero values.  */
void
init_sal (struct symtab_and_line *sal)
{
  sal->symtab = 0;
  sal->section = 0;
  sal->line = 0;
  sal->pc = 0;
  sal->end = 0;
  sal->explicit_pc = 0;
  sal->explicit_line = 0;
}


/* Return 1 if the two sections are the same, or if they could
   plausibly be copies of each other, one in an original object
   file and another in a separated debug file.  */

int
matching_obj_sections (struct obj_section *obj_first,
		       struct obj_section *obj_second)
{
  asection *first = obj_first? obj_first->the_bfd_section : NULL;
  asection *second = obj_second? obj_second->the_bfd_section : NULL;
  struct objfile *obj;

  /* If they're the same section, then they match.  */
  if (first == second)
    return 1;

  /* If either is NULL, give up.  */
  if (first == NULL || second == NULL)
    return 0;

  /* This doesn't apply to absolute symbols.  */
  if (first->owner == NULL || second->owner == NULL)
    return 0;

  /* If they're in the same object file, they must be different sections.  */
  if (first->owner == second->owner)
    return 0;

  /* Check whether the two sections are potentially corresponding.  They must
     have the same size, address, and name.  We can't compare section indexes,
     which would be more reliable, because some sections may have been
     stripped.  */
  if (bfd_get_section_size (first) != bfd_get_section_size (second))
    return 0;

  /* In-memory addresses may start at a different offset, relativize them.  */
  if (bfd_get_section_vma (first->owner, first)
      - bfd_get_start_address (first->owner)
      != bfd_get_section_vma (second->owner, second)
	 - bfd_get_start_address (second->owner))
    return 0;

  if (bfd_get_section_name (first->owner, first) == NULL
      || bfd_get_section_name (second->owner, second) == NULL
      || strcmp (bfd_get_section_name (first->owner, first),
		 bfd_get_section_name (second->owner, second)) != 0)
    return 0;

  /* Otherwise check that they are in corresponding objfiles.  */

  ALL_OBJFILES (obj)
    if (obj->obfd == first->owner)
      break;
  gdb_assert (obj != NULL);

  if (obj->separate_debug_objfile != NULL
      && obj->separate_debug_objfile->obfd == second->owner)
    return 1;
  if (obj->separate_debug_objfile_backlink != NULL
      && obj->separate_debug_objfile_backlink->obfd == second->owner)
    return 1;

  return 0;
}

/* Find which partial symtab contains PC and SECTION starting at psymtab PST.
   We may find a different psymtab than PST.  See FIND_PC_SECT_PSYMTAB.  */

struct partial_symtab *
find_pc_sect_psymtab_closer (CORE_ADDR pc, struct obj_section *section,
			     struct partial_symtab *pst,
			     struct minimal_symbol *msymbol)
{
  struct objfile *objfile = pst->objfile;
  struct partial_symtab *tpst;
  struct partial_symtab *best_pst = pst;
  CORE_ADDR best_addr = pst->textlow;

  /* An objfile that has its functions reordered might have
     many partial symbol tables containing the PC, but
     we want the partial symbol table that contains the
     function containing the PC.  */
  if (!(objfile->flags & OBJF_REORDERED) &&
      section == 0)	/* can't validate section this way */
    return pst;

  if (msymbol == NULL)
    return (pst);

  /* The code range of partial symtabs sometimes overlap, so, in
     the loop below, we need to check all partial symtabs and
     find the one that fits better for the given PC address. We
     select the partial symtab that contains a symbol whose
     address is closest to the PC address.  By closest we mean
     that find_pc_sect_symbol returns the symbol with address
     that is closest and still less than the given PC.  */
  for (tpst = pst; tpst != NULL; tpst = tpst->next)
    {
      if (pc >= tpst->textlow && pc < tpst->texthigh)
	{
	  struct partial_symbol *p;
	  CORE_ADDR this_addr;

	  /* NOTE: This assumes that every psymbol has a
	     corresponding msymbol, which is not necessarily
	     true; the debug info might be much richer than the
	     object's symbol table.  */
	  p = find_pc_sect_psymbol (tpst, pc, section);
	  if (p != NULL
	      && SYMBOL_VALUE_ADDRESS (p)
	      == SYMBOL_VALUE_ADDRESS (msymbol))
	    return tpst;

	  /* Also accept the textlow value of a psymtab as a
	     "symbol", to provide some support for partial
	     symbol tables with line information but no debug
	     symbols (e.g. those produced by an assembler).  */
	  if (p != NULL)
	    this_addr = SYMBOL_VALUE_ADDRESS (p);
	  else
	    this_addr = tpst->textlow;

	  /* Check whether it is closer than our current
	     BEST_ADDR.  Since this symbol address is
	     necessarily lower or equal to PC, the symbol closer
	     to PC is the symbol which address is the highest.
	     This way we return the psymtab which contains such
	     best match symbol. This can help in cases where the
	     symbol information/debuginfo is not complete, like
	     for instance on IRIX6 with gcc, where no debug info
	     is emitted for statics. (See also the nodebug.exp
	     testcase.) */
	  if (this_addr > best_addr)
	    {
	      best_addr = this_addr;
	      best_pst = tpst;
	    }
	}
    }
  return best_pst;
}

/* Find which partial symtab contains PC and SECTION.  Return 0 if
   none.  We return the psymtab that contains a symbol whose address
   exactly matches PC, or, if we cannot find an exact match, the
   psymtab that contains a symbol whose address is closest to PC.  */
struct partial_symtab *
find_pc_sect_psymtab (CORE_ADDR pc, struct obj_section *section)
{
  struct objfile *objfile;
  struct minimal_symbol *msymbol;

  /* If we know that this is not a text address, return failure.  This is
     necessary because we loop based on texthigh and textlow, which do
     not include the data ranges.  */
  msymbol = lookup_minimal_symbol_by_pc_section (pc, section);
  if (msymbol
      && (msymbol->type == mst_data
	  || msymbol->type == mst_bss
	  || msymbol->type == mst_abs
	  || msymbol->type == mst_file_data
	  || msymbol->type == mst_file_bss))
    return NULL;

  /* Try just the PSYMTABS_ADDRMAP mapping first as it has better granularity
     than the later used TEXTLOW/TEXTHIGH one.  */

  ALL_OBJFILES (objfile)
    if (objfile->psymtabs_addrmap != NULL)
      {
	struct partial_symtab *pst;

	pst = addrmap_find (objfile->psymtabs_addrmap, pc);
	if (pst != NULL)
	  {
	    /* FIXME: addrmaps currently do not handle overlayed sections,
	       so fall back to the non-addrmap case if we're debugging 
	       overlays and the addrmap returned the wrong section.  */
	    if (overlay_debugging && msymbol && section)
	      {
		struct partial_symbol *p;
		/* NOTE: This assumes that every psymbol has a
		   corresponding msymbol, which is not necessarily
		   true; the debug info might be much richer than the
		   object's symbol table.  */
		p = find_pc_sect_psymbol (pst, pc, section);
		if (!p
		    || SYMBOL_VALUE_ADDRESS (p)
		       != SYMBOL_VALUE_ADDRESS (msymbol))
		  continue;
	      }

	    /* We do not try to call FIND_PC_SECT_PSYMTAB_CLOSER as
	       PSYMTABS_ADDRMAP we used has already the best 1-byte
	       granularity and FIND_PC_SECT_PSYMTAB_CLOSER may mislead us into
	       a worse chosen section due to the TEXTLOW/TEXTHIGH ranges
	       overlap.  */

	    return pst;
	  }
      }

  /* Existing PSYMTABS_ADDRMAP mapping is present even for PARTIAL_SYMTABs
     which still have no corresponding full SYMTABs read.  But it is not
     present for non-DWARF2 debug infos not supporting PSYMTABS_ADDRMAP in GDB
     so far.  */

  ALL_OBJFILES (objfile)
    {
      struct partial_symtab *pst;

      /* Check even OBJFILE with non-zero PSYMTABS_ADDRMAP as only several of
	 its CUs may be missing in PSYMTABS_ADDRMAP as they may be varying
	 debug info type in single OBJFILE.  */

      ALL_OBJFILE_PSYMTABS (objfile, pst)
	if (pc >= pst->textlow && pc < pst->texthigh)
	  {
	    struct partial_symtab *best_pst;

	    best_pst = find_pc_sect_psymtab_closer (pc, section, pst,
						    msymbol);
	    if (best_pst != NULL)
	      return best_pst;
	  }
    }

  return NULL;
}

/* Find which partial symtab contains PC.  Return 0 if none. 
   Backward compatibility, no section */

struct partial_symtab *
find_pc_psymtab (CORE_ADDR pc)
{
  return find_pc_sect_psymtab (pc, find_pc_mapped_section (pc));
}

/* Find which partial symbol within a psymtab matches PC and SECTION.  
   Return 0 if none.  Check all psymtabs if PSYMTAB is 0.  */

struct partial_symbol *
find_pc_sect_psymbol (struct partial_symtab *psymtab, CORE_ADDR pc,
		      struct obj_section *section)
{
  struct partial_symbol *best = NULL, *p, **pp;
  CORE_ADDR best_pc;

  if (!psymtab)
    psymtab = find_pc_sect_psymtab (pc, section);
  if (!psymtab)
    return 0;

  /* Cope with programs that start at address 0 */
  best_pc = (psymtab->textlow != 0) ? psymtab->textlow - 1 : 0;

  /* Search the global symbols as well as the static symbols, so that
     find_pc_partial_function doesn't use a minimal symbol and thus
     cache a bad endaddr.  */
  for (pp = psymtab->objfile->global_psymbols.list + psymtab->globals_offset;
    (pp - (psymtab->objfile->global_psymbols.list + psymtab->globals_offset)
     < psymtab->n_global_syms);
       pp++)
    {
      p = *pp;
      if (SYMBOL_DOMAIN (p) == VAR_DOMAIN
	  && SYMBOL_CLASS (p) == LOC_BLOCK
	  && pc >= SYMBOL_VALUE_ADDRESS (p)
	  && (SYMBOL_VALUE_ADDRESS (p) > best_pc
	      || (psymtab->textlow == 0
		  && best_pc == 0 && SYMBOL_VALUE_ADDRESS (p) == 0)))
	{
	  if (section)		/* match on a specific section */
	    {
	      fixup_psymbol_section (p, psymtab->objfile);
	      if (!matching_obj_sections (SYMBOL_OBJ_SECTION (p), section))
		continue;
	    }
	  best_pc = SYMBOL_VALUE_ADDRESS (p);
	  best = p;
	}
    }

  for (pp = psymtab->objfile->static_psymbols.list + psymtab->statics_offset;
    (pp - (psymtab->objfile->static_psymbols.list + psymtab->statics_offset)
     < psymtab->n_static_syms);
       pp++)
    {
      p = *pp;
      if (SYMBOL_DOMAIN (p) == VAR_DOMAIN
	  && SYMBOL_CLASS (p) == LOC_BLOCK
	  && pc >= SYMBOL_VALUE_ADDRESS (p)
	  && (SYMBOL_VALUE_ADDRESS (p) > best_pc
	      || (psymtab->textlow == 0
		  && best_pc == 0 && SYMBOL_VALUE_ADDRESS (p) == 0)))
	{
	  if (section)		/* match on a specific section */
	    {
	      fixup_psymbol_section (p, psymtab->objfile);
	      if (!matching_obj_sections (SYMBOL_OBJ_SECTION (p), section))
		continue;
	    }
	  best_pc = SYMBOL_VALUE_ADDRESS (p);
	  best = p;
	}
    }

  return best;
}

/* Find which partial symbol within a psymtab matches PC.  Return 0 if none.  
   Check all psymtabs if PSYMTAB is 0.  Backwards compatibility, no section. */

struct partial_symbol *
find_pc_psymbol (struct partial_symtab *psymtab, CORE_ADDR pc)
{
  return find_pc_sect_psymbol (psymtab, pc, find_pc_mapped_section (pc));
}

/* Debug symbols usually don't have section information.  We need to dig that
   out of the minimal symbols and stash that in the debug symbol.  */

static void
fixup_section (struct general_symbol_info *ginfo,
	       CORE_ADDR addr, struct objfile *objfile)
{
  struct minimal_symbol *msym;

  /* First, check whether a minimal symbol with the same name exists
     and points to the same address.  The address check is required
     e.g. on PowerPC64, where the minimal symbol for a function will
     point to the function descriptor, while the debug symbol will
     point to the actual function code.  */
  msym = lookup_minimal_symbol_by_pc_name (addr, ginfo->name, objfile);
  if (msym)
    {
      ginfo->obj_section = SYMBOL_OBJ_SECTION (msym);
      ginfo->section = SYMBOL_SECTION (msym);
    }
  else
    {
      /* Static, function-local variables do appear in the linker
	 (minimal) symbols, but are frequently given names that won't
	 be found via lookup_minimal_symbol().  E.g., it has been
	 observed in frv-uclinux (ELF) executables that a static,
	 function-local variable named "foo" might appear in the
	 linker symbols as "foo.6" or "foo.3".  Thus, there is no
	 point in attempting to extend the lookup-by-name mechanism to
	 handle this case due to the fact that there can be multiple
	 names.
	 
	 So, instead, search the section table when lookup by name has
	 failed.  The ``addr'' and ``endaddr'' fields may have already
	 been relocated.  If so, the relocation offset (i.e. the
	 ANOFFSET value) needs to be subtracted from these values when
	 performing the comparison.  We unconditionally subtract it,
	 because, when no relocation has been performed, the ANOFFSET
	 value will simply be zero.
	 
	 The address of the symbol whose section we're fixing up HAS
	 NOT BEEN adjusted (relocated) yet.  It can't have been since
	 the section isn't yet known and knowing the section is
	 necessary in order to add the correct relocation value.  In
	 other words, we wouldn't even be in this function (attempting
	 to compute the section) if it were already known.

	 Note that it is possible to search the minimal symbols
	 (subtracting the relocation value if necessary) to find the
	 matching minimal symbol, but this is overkill and much less
	 efficient.  It is not necessary to find the matching minimal
	 symbol, only its section.  
	 
	 Note that this technique (of doing a section table search)
	 can fail when unrelocated section addresses overlap.  For
	 this reason, we still attempt a lookup by name prior to doing
	 a search of the section table.  */
	 
      struct obj_section *s;
      ALL_OBJFILE_OSECTIONS (objfile, s)
	{
	  int idx = s->the_bfd_section->index;
	  CORE_ADDR offset = ANOFFSET (objfile->section_offsets, idx);

	  if (obj_section_addr (s) - offset <= addr
	      && addr < obj_section_endaddr (s) - offset)
	    {
	      ginfo->obj_section = s;
	      ginfo->section = idx;
	      return;
	    }
	}
    }
}

struct symbol *
fixup_symbol_section (struct symbol *sym, struct objfile *objfile)
{
  CORE_ADDR addr;

  if (!sym)
    return NULL;

  if (SYMBOL_OBJ_SECTION (sym))
    return sym;

  /* We either have an OBJFILE, or we can get at it from the sym's
     symtab.  Anything else is a bug.  */
  gdb_assert (objfile || SYMBOL_SYMTAB (sym));

  if (objfile == NULL)
    objfile = SYMBOL_SYMTAB (sym)->objfile;

  /* We should have an objfile by now.  */
  gdb_assert (objfile);

  switch (SYMBOL_CLASS (sym))
    {
    case LOC_STATIC:
    case LOC_LABEL:
      addr = SYMBOL_VALUE_ADDRESS (sym);
      break;
    case LOC_BLOCK:
      addr = BLOCK_START (SYMBOL_BLOCK_VALUE (sym));
      break;

    default:
      /* Nothing else will be listed in the minsyms -- no use looking
	 it up.  */
      return sym;
    }

  fixup_section (&sym->ginfo, addr, objfile);

  return sym;
}

struct partial_symbol *
fixup_psymbol_section (struct partial_symbol *psym, struct objfile *objfile)
{
  CORE_ADDR addr;

  if (!psym)
    return NULL;

  if (SYMBOL_OBJ_SECTION (psym))
    return psym;

  gdb_assert (objfile);

  switch (SYMBOL_CLASS (psym))
    {
    case LOC_STATIC:
    case LOC_LABEL:
    case LOC_BLOCK:
      addr = SYMBOL_VALUE_ADDRESS (psym);
      break;
    default:
      /* Nothing else will be listed in the minsyms -- no use looking
	 it up.  */
      return psym;
    }

  fixup_section (&psym->ginfo, addr, objfile);

  return psym;
}

/* Find the definition for a specified symbol name NAME
   in domain DOMAIN, visible from lexical block BLOCK.
   Returns the struct symbol pointer, or zero if no symbol is found.
   C++: if IS_A_FIELD_OF_THIS is nonzero on entry, check to see if
   NAME is a field of the current implied argument `this'.  If so set
   *IS_A_FIELD_OF_THIS to 1, otherwise set it to zero. 
   BLOCK_FOUND is set to the block in which NAME is found (in the case of
   a field of `this', value_of_this sets BLOCK_FOUND to the proper value.) */

/* This function has a bunch of loops in it and it would seem to be
   attractive to put in some QUIT's (though I'm not really sure
   whether it can run long enough to be really important).  But there
   are a few calls for which it would appear to be bad news to quit
   out of here: find_proc_desc in alpha-tdep.c and mips-tdep.c.  (Note
   that there is C++ code below which can error(), but that probably
   doesn't affect these calls since they are looking for a known
   variable and thus can probably assume it will never hit the C++
   code).  */

struct symbol *
lookup_symbol_in_language (const char *name, const struct block *block,
			   const domain_enum domain, enum language lang,
			   int *is_a_field_of_this)
{
  char *demangled_name = NULL;
  const char *modified_name = NULL;
  const char *mangled_name = NULL;
  int needtofreename = 0;
  struct symbol *returnval;

  modified_name = name;

  /* If we are using C++ or Java, demangle the name before doing a lookup, so
     we can always binary search. */
  if (lang == language_cplus)
    {
      demangled_name = cplus_demangle (name, DMGL_ANSI | DMGL_PARAMS);
      if (demangled_name)
	{
	  mangled_name = name;
	  modified_name = demangled_name;
	  needtofreename = 1;
	}
    }
  else if (lang == language_java)
    {
      demangled_name = cplus_demangle (name, 
		      		       DMGL_ANSI | DMGL_PARAMS | DMGL_JAVA);
      if (demangled_name)
	{
	  mangled_name = name;
	  modified_name = demangled_name;
	  needtofreename = 1;
	}
    }

  if (case_sensitivity == case_sensitive_off)
    {
      char *copy;
      int len, i;

      len = strlen (name);
      copy = (char *) alloca (len + 1);
      for (i= 0; i < len; i++)
        copy[i] = tolower (name[i]);
      copy[len] = 0;
      modified_name = copy;
    }

  returnval = lookup_symbol_aux (modified_name, mangled_name, block,
				 domain, lang, is_a_field_of_this);
  if (needtofreename)
    xfree (demangled_name);

  return returnval;	 
}

/* Behave like lookup_symbol_in_language, but performed with the
   current language.  */

struct symbol *
lookup_symbol (const char *name, const struct block *block,
	       domain_enum domain, int *is_a_field_of_this)
{
  return lookup_symbol_in_language (name, block, domain,
				    current_language->la_language,
				    is_a_field_of_this);
}

/* Behave like lookup_symbol except that NAME is the natural name
   of the symbol that we're looking for and, if LINKAGE_NAME is
   non-NULL, ensure that the symbol's linkage name matches as
   well.  */

static struct symbol *
lookup_symbol_aux (const char *name, const char *linkage_name,
		   const struct block *block, const domain_enum domain,
		   enum language language, int *is_a_field_of_this)
{
  struct symbol *sym;
  const struct language_defn *langdef;

  /* Make sure we do something sensible with is_a_field_of_this, since
     the callers that set this parameter to some non-null value will
     certainly use it later and expect it to be either 0 or 1.
     If we don't set it, the contents of is_a_field_of_this are
     undefined.  */
  if (is_a_field_of_this != NULL)
    *is_a_field_of_this = 0;

  /* Search specified block and its superiors.  Don't search
     STATIC_BLOCK or GLOBAL_BLOCK.  */

  sym = lookup_symbol_aux_local (name, linkage_name, block, domain);
  if (sym != NULL)
    return sym;

  /* If requested to do so by the caller and if appropriate for LANGUAGE,
     check to see if NAME is a field of `this'. */

  langdef = language_def (language);

  if (langdef->la_name_of_this != NULL && is_a_field_of_this != NULL
      && block != NULL)
    {
      struct symbol *sym = NULL;
      /* 'this' is only defined in the function's block, so find the
	 enclosing function block.  */
      for (; block && !BLOCK_FUNCTION (block); 
	   block = BLOCK_SUPERBLOCK (block));

      if (block && !dict_empty (BLOCK_DICT (block)))
	sym = lookup_block_symbol (block, langdef->la_name_of_this,
				   NULL, VAR_DOMAIN);
      if (sym)
	{
	  struct type *t = sym->type;
	  
	  /* I'm not really sure that type of this can ever
	     be typedefed; just be safe.  */
	  CHECK_TYPEDEF (t);
	  if (TYPE_CODE (t) == TYPE_CODE_PTR
	      || TYPE_CODE (t) == TYPE_CODE_REF)
	    t = TYPE_TARGET_TYPE (t);
	  
	  if (TYPE_CODE (t) != TYPE_CODE_STRUCT
	      && TYPE_CODE (t) != TYPE_CODE_UNION)
	    error (_("Internal error: `%s' is not an aggregate"), 
		   langdef->la_name_of_this);
	  
	  if (check_field (t, name))
	    {
	      *is_a_field_of_this = 1;
	      return NULL;
	    }
	}
    }

  /* Now do whatever is appropriate for LANGUAGE to look
     up static and global variables.  */

  sym = langdef->la_lookup_symbol_nonlocal (name, linkage_name, block, domain);
  if (sym != NULL)
    return sym;

  /* Now search all static file-level symbols.  Not strictly correct,
     but more useful than an error.  Do the symtabs first, then check
     the psymtabs.  If a psymtab indicates the existence of the
     desired name as a file-level static, then do psymtab-to-symtab
     conversion on the fly and return the found symbol. */

  sym = lookup_symbol_aux_symtabs (STATIC_BLOCK, name, linkage_name, domain);
  if (sym != NULL)
    return sym;
  
  sym = lookup_symbol_aux_psymtabs (STATIC_BLOCK, name, linkage_name, domain);
  if (sym != NULL)
    return sym;

  return NULL;
}

/* Check to see if the symbol is defined in BLOCK or its superiors.
   Don't search STATIC_BLOCK or GLOBAL_BLOCK.  */

static struct symbol *
lookup_symbol_aux_local (const char *name, const char *linkage_name,
			 const struct block *block,
			 const domain_enum domain)
{
  struct symbol *sym;
  const struct block *static_block = block_static_block (block);

  /* Check if either no block is specified or it's a global block.  */

  if (static_block == NULL)
    return NULL;

  while (block != static_block)
    {
      sym = lookup_symbol_aux_block (name, linkage_name, block, domain);
      if (sym != NULL)
	return sym;
      block = BLOCK_SUPERBLOCK (block);
    }

  /* We've reached the static block without finding a result.  */

  return NULL;
}

/* Look up OBJFILE to BLOCK.  */

static struct objfile *
lookup_objfile_from_block (const struct block *block)
{
  struct objfile *obj;
  struct symtab *s;

  if (block == NULL)
    return NULL;

  block = block_global_block (block);
  /* Go through SYMTABS.  */
  ALL_SYMTABS (obj, s)
    if (block == BLOCKVECTOR_BLOCK (BLOCKVECTOR (s), GLOBAL_BLOCK))
      return obj;

  return NULL;
}

/* Look up a symbol in a block; if found, fixup the symbol, and set
   block_found appropriately.  */

struct symbol *
lookup_symbol_aux_block (const char *name, const char *linkage_name,
			 const struct block *block,
			 const domain_enum domain)
{
  struct symbol *sym;

  sym = lookup_block_symbol (block, name, linkage_name, domain);
  if (sym)
    {
      block_found = block;
      return fixup_symbol_section (sym, NULL);
    }

  return NULL;
}

/* Check all global symbols in OBJFILE in symtabs and
   psymtabs.  */

struct symbol *
lookup_global_symbol_from_objfile (const struct objfile *objfile,
				   const char *name,
				   const char *linkage_name,
				   const domain_enum domain)
{
  struct symbol *sym;
  struct blockvector *bv;
  const struct block *block;
  struct symtab *s;
  struct partial_symtab *ps;

  /* Go through symtabs.  */
  ALL_OBJFILE_SYMTABS (objfile, s)
  {
    bv = BLOCKVECTOR (s);
    block = BLOCKVECTOR_BLOCK (bv, GLOBAL_BLOCK);
    sym = lookup_block_symbol (block, name, linkage_name, domain);
    if (sym)
      {
	block_found = block;
	return fixup_symbol_section (sym, (struct objfile *)objfile);
      }
  }

  /* Now go through psymtabs.  */
  ALL_OBJFILE_PSYMTABS (objfile, ps)
  {
    if (!ps->readin
	&& lookup_partial_symbol (ps, name, linkage_name,
				  1, domain))
      {
	s = PSYMTAB_TO_SYMTAB (ps);
	bv = BLOCKVECTOR (s);
	block = BLOCKVECTOR_BLOCK (bv, GLOBAL_BLOCK);
	sym = lookup_block_symbol (block, name, linkage_name, domain);
	return fixup_symbol_section (sym, (struct objfile *)objfile);
      }
  }

  if (objfile->separate_debug_objfile)
    return lookup_global_symbol_from_objfile (objfile->separate_debug_objfile,
					      name, linkage_name, domain);

  return NULL;
}

/* Check to see if the symbol is defined in one of the symtabs.
   BLOCK_INDEX should be either GLOBAL_BLOCK or STATIC_BLOCK,
   depending on whether or not we want to search global symbols or
   static symbols.  */

static struct symbol *
lookup_symbol_aux_symtabs (int block_index,
			   const char *name, const char *linkage_name,
			   const domain_enum domain)
{
  struct symbol *sym;
  struct objfile *objfile;
  struct blockvector *bv;
  const struct block *block;
  struct symtab *s;

  ALL_PRIMARY_SYMTABS (objfile, s)
  {
    bv = BLOCKVECTOR (s);
    block = BLOCKVECTOR_BLOCK (bv, block_index);
    sym = lookup_block_symbol (block, name, linkage_name, domain);
    if (sym)
      {
	block_found = block;
	return fixup_symbol_section (sym, objfile);
      }
  }

  return NULL;
}

/* Check to see if the symbol is defined in one of the partial
   symtabs.  BLOCK_INDEX should be either GLOBAL_BLOCK or
   STATIC_BLOCK, depending on whether or not we want to search global
   symbols or static symbols.  */

static struct symbol *
lookup_symbol_aux_psymtabs (int block_index, const char *name,
			    const char *linkage_name,
			    const domain_enum domain)
{
  struct symbol *sym;
  struct objfile *objfile;
  struct blockvector *bv;
  const struct block *block;
  struct partial_symtab *ps;
  struct symtab *s;
  const int psymtab_index = (block_index == GLOBAL_BLOCK ? 1 : 0);

  ALL_PSYMTABS (objfile, ps)
  {
    if (!ps->readin
	&& lookup_partial_symbol (ps, name, linkage_name,
				  psymtab_index, domain))
      {
	s = PSYMTAB_TO_SYMTAB (ps);
	bv = BLOCKVECTOR (s);
	block = BLOCKVECTOR_BLOCK (bv, block_index);
	sym = lookup_block_symbol (block, name, linkage_name, domain);
	if (!sym)
	  {
	    /* This shouldn't be necessary, but as a last resort try
	       looking in the statics even though the psymtab claimed
	       the symbol was global, or vice-versa. It's possible
	       that the psymtab gets it wrong in some cases.  */

	    /* FIXME: carlton/2002-09-30: Should we really do that?
	       If that happens, isn't it likely to be a GDB error, in
	       which case we should fix the GDB error rather than
	       silently dealing with it here?  So I'd vote for
	       removing the check for the symbol in the other
	       block.  */
	    block = BLOCKVECTOR_BLOCK (bv,
				       block_index == GLOBAL_BLOCK ?
				       STATIC_BLOCK : GLOBAL_BLOCK);
	    sym = lookup_block_symbol (block, name, linkage_name, domain);
	    if (!sym)
	      error (_("Internal: %s symbol `%s' found in %s psymtab but not in symtab.\n%s may be an inlined function, or may be a template function\n(if a template, try specifying an instantiation: %s<type>)."),
		     block_index == GLOBAL_BLOCK ? "global" : "static",
		     name, ps->filename, name, name);
	  }
	return fixup_symbol_section (sym, objfile);
      }
  }

  return NULL;
}

/* A default version of lookup_symbol_nonlocal for use by languages
   that can't think of anything better to do.  This implements the C
   lookup rules.  */

struct symbol *
basic_lookup_symbol_nonlocal (const char *name,
			      const char *linkage_name,
			      const struct block *block,
			      const domain_enum domain)
{
  struct symbol *sym;

  /* NOTE: carlton/2003-05-19: The comments below were written when
     this (or what turned into this) was part of lookup_symbol_aux;
     I'm much less worried about these questions now, since these
     decisions have turned out well, but I leave these comments here
     for posterity.  */

  /* NOTE: carlton/2002-12-05: There is a question as to whether or
     not it would be appropriate to search the current global block
     here as well.  (That's what this code used to do before the
     is_a_field_of_this check was moved up.)  On the one hand, it's
     redundant with the lookup_symbol_aux_symtabs search that happens
     next.  On the other hand, if decode_line_1 is passed an argument
     like filename:var, then the user presumably wants 'var' to be
     searched for in filename.  On the third hand, there shouldn't be
     multiple global variables all of which are named 'var', and it's
     not like decode_line_1 has ever restricted its search to only
     global variables in a single filename.  All in all, only
     searching the static block here seems best: it's correct and it's
     cleanest.  */

  /* NOTE: carlton/2002-12-05: There's also a possible performance
     issue here: if you usually search for global symbols in the
     current file, then it would be slightly better to search the
     current global block before searching all the symtabs.  But there
     are other factors that have a much greater effect on performance
     than that one, so I don't think we should worry about that for
     now.  */

  sym = lookup_symbol_static (name, linkage_name, block, domain);
  if (sym != NULL)
    return sym;

  return lookup_symbol_global (name, linkage_name, block, domain);
}

/* Lookup a symbol in the static block associated to BLOCK, if there
   is one; do nothing if BLOCK is NULL or a global block.  */

struct symbol *
lookup_symbol_static (const char *name,
		      const char *linkage_name,
		      const struct block *block,
		      const domain_enum domain)
{
  const struct block *static_block = block_static_block (block);

  if (static_block != NULL)
    return lookup_symbol_aux_block (name, linkage_name, static_block, domain);
  else
    return NULL;
}

/* Lookup a symbol in all files' global blocks (searching psymtabs if
   necessary).  */

struct symbol *
lookup_symbol_global (const char *name,
		      const char *linkage_name,
		      const struct block *block,
		      const domain_enum domain)
{
  struct symbol *sym = NULL;
  struct objfile *objfile = NULL;

  /* Call library-specific lookup procedure.  */
  objfile = lookup_objfile_from_block (block);
  if (objfile != NULL)
    sym = solib_global_lookup (objfile, name, linkage_name, domain);
  if (sym != NULL)
    return sym;

  sym = lookup_symbol_aux_symtabs (GLOBAL_BLOCK, name, linkage_name, domain);
  if (sym != NULL)
    return sym;

  return lookup_symbol_aux_psymtabs (GLOBAL_BLOCK, name, linkage_name, domain);
}

int
symbol_matches_domain (enum language symbol_language, 
		       domain_enum symbol_domain,
		       domain_enum domain)
{
  /* For C++ "struct foo { ... }" also defines a typedef for "foo".  
     A Java class declaration also defines a typedef for the class.
     Similarly, any Ada type declaration implicitly defines a typedef.  */
  if (symbol_language == language_cplus
      || symbol_language == language_java
      || symbol_language == language_ada)
    {
      if ((domain == VAR_DOMAIN || domain == STRUCT_DOMAIN)
	  && symbol_domain == STRUCT_DOMAIN)
	return 1;
    }
  /* For all other languages, strict match is required.  */
  return (symbol_domain == domain);
}

/* Look, in partial_symtab PST, for symbol whose natural name is NAME.
   If LINKAGE_NAME is non-NULL, check in addition that the symbol's
   linkage name matches it.  Check the global symbols if GLOBAL, the
   static symbols if not */

struct partial_symbol *
lookup_partial_symbol (struct partial_symtab *pst, const char *name,
		       const char *linkage_name, int global,
		       domain_enum domain)
{
  struct partial_symbol *temp;
  struct partial_symbol **start, **psym;
  struct partial_symbol **top, **real_top, **bottom, **center;
  int length = (global ? pst->n_global_syms : pst->n_static_syms);
  int do_linear_search = 1;
  
  if (length == 0)
    {
      return (NULL);
    }
  start = (global ?
	   pst->objfile->global_psymbols.list + pst->globals_offset :
	   pst->objfile->static_psymbols.list + pst->statics_offset);
  
  if (global)			/* This means we can use a binary search. */
    {
      do_linear_search = 0;

      /* Binary search.  This search is guaranteed to end with center
         pointing at the earliest partial symbol whose name might be
         correct.  At that point *all* partial symbols with an
         appropriate name will be checked against the correct
         domain.  */

      bottom = start;
      top = start + length - 1;
      real_top = top;
      while (top > bottom)
	{
	  center = bottom + (top - bottom) / 2;
	  if (!(center < top))
	    internal_error (__FILE__, __LINE__, _("failed internal consistency check"));
	  if (!do_linear_search
	      && (SYMBOL_LANGUAGE (*center) == language_java))
	    {
	      do_linear_search = 1;
	    }
	  if (strcmp_iw_ordered (SYMBOL_SEARCH_NAME (*center), name) >= 0)
	    {
	      top = center;
	    }
	  else
	    {
	      bottom = center + 1;
	    }
	}
      if (!(top == bottom))
	internal_error (__FILE__, __LINE__, _("failed internal consistency check"));

      while (top <= real_top
	     && (linkage_name != NULL
		 ? strcmp (SYMBOL_LINKAGE_NAME (*top), linkage_name) == 0
		 : SYMBOL_MATCHES_SEARCH_NAME (*top,name)))
	{
	  if (symbol_matches_domain (SYMBOL_LANGUAGE (*top),
				     SYMBOL_DOMAIN (*top), domain))
	    return (*top);
	  top++;
	}
    }

  /* Can't use a binary search or else we found during the binary search that
     we should also do a linear search. */

  if (do_linear_search)
    {			
      for (psym = start; psym < start + length; psym++)
	{
	  if (symbol_matches_domain (SYMBOL_LANGUAGE (*psym), 
				     SYMBOL_DOMAIN (*psym), domain))
	    {
	      if (linkage_name != NULL
		  ? strcmp (SYMBOL_LINKAGE_NAME (*psym), linkage_name) == 0
		  : SYMBOL_MATCHES_SEARCH_NAME (*psym, name))
		{
		  return (*psym);
		}
	    }
	}
    }

  return (NULL);
}

/* Look up a type named NAME in the struct_domain.  The type returned
   must not be opaque -- i.e., must have at least one field
   defined.  */

struct type *
lookup_transparent_type (const char *name)
{
  return current_language->la_lookup_transparent_type (name);
}

/* The standard implementation of lookup_transparent_type.  This code
   was modeled on lookup_symbol -- the parts not relevant to looking
   up types were just left out.  In particular it's assumed here that
   types are available in struct_domain and only at file-static or
   global blocks.  */

struct type *
basic_lookup_transparent_type (const char *name)
{
  struct symbol *sym;
  struct symtab *s = NULL;
  struct partial_symtab *ps;
  struct blockvector *bv;
  struct objfile *objfile;
  struct block *block;

  /* Now search all the global symbols.  Do the symtab's first, then
     check the psymtab's. If a psymtab indicates the existence
     of the desired name as a global, then do psymtab-to-symtab
     conversion on the fly and return the found symbol.  */

  ALL_PRIMARY_SYMTABS (objfile, s)
  {
    bv = BLOCKVECTOR (s);
    block = BLOCKVECTOR_BLOCK (bv, GLOBAL_BLOCK);
    sym = lookup_block_symbol (block, name, NULL, STRUCT_DOMAIN);
    if (sym && !TYPE_IS_OPAQUE (SYMBOL_TYPE (sym)))
      {
	return SYMBOL_TYPE (sym);
      }
  }

  ALL_PSYMTABS (objfile, ps)
  {
    if (!ps->readin && lookup_partial_symbol (ps, name, NULL,
					      1, STRUCT_DOMAIN))
      {
	s = PSYMTAB_TO_SYMTAB (ps);
	bv = BLOCKVECTOR (s);
	block = BLOCKVECTOR_BLOCK (bv, GLOBAL_BLOCK);
	sym = lookup_block_symbol (block, name, NULL, STRUCT_DOMAIN);
	if (!sym)
	  {
	    /* This shouldn't be necessary, but as a last resort
	     * try looking in the statics even though the psymtab
	     * claimed the symbol was global. It's possible that
	     * the psymtab gets it wrong in some cases.
	     */
	    block = BLOCKVECTOR_BLOCK (bv, STATIC_BLOCK);
	    sym = lookup_block_symbol (block, name, NULL, STRUCT_DOMAIN);
	    if (!sym)
	      error (_("Internal: global symbol `%s' found in %s psymtab but not in symtab.\n\
%s may be an inlined function, or may be a template function\n\
(if a template, try specifying an instantiation: %s<type>)."),
		     name, ps->filename, name, name);
	  }
	if (!TYPE_IS_OPAQUE (SYMBOL_TYPE (sym)))
	  return SYMBOL_TYPE (sym);
      }
  }

  /* Now search the static file-level symbols.
     Not strictly correct, but more useful than an error.
     Do the symtab's first, then
     check the psymtab's. If a psymtab indicates the existence
     of the desired name as a file-level static, then do psymtab-to-symtab
     conversion on the fly and return the found symbol.
   */

  ALL_PRIMARY_SYMTABS (objfile, s)
  {
    bv = BLOCKVECTOR (s);
    block = BLOCKVECTOR_BLOCK (bv, STATIC_BLOCK);
    sym = lookup_block_symbol (block, name, NULL, STRUCT_DOMAIN);
    if (sym && !TYPE_IS_OPAQUE (SYMBOL_TYPE (sym)))
      {
	return SYMBOL_TYPE (sym);
      }
  }

  ALL_PSYMTABS (objfile, ps)
  {
    if (!ps->readin && lookup_partial_symbol (ps, name, NULL, 0, STRUCT_DOMAIN))
      {
	s = PSYMTAB_TO_SYMTAB (ps);
	bv = BLOCKVECTOR (s);
	block = BLOCKVECTOR_BLOCK (bv, STATIC_BLOCK);
	sym = lookup_block_symbol (block, name, NULL, STRUCT_DOMAIN);
	if (!sym)
	  {
	    /* This shouldn't be necessary, but as a last resort
	     * try looking in the globals even though the psymtab
	     * claimed the symbol was static. It's possible that
	     * the psymtab gets it wrong in some cases.
	     */
	    block = BLOCKVECTOR_BLOCK (bv, GLOBAL_BLOCK);
	    sym = lookup_block_symbol (block, name, NULL, STRUCT_DOMAIN);
	    if (!sym)
	      error (_("Internal: static symbol `%s' found in %s psymtab but not in symtab.\n\
%s may be an inlined function, or may be a template function\n\
(if a template, try specifying an instantiation: %s<type>)."),
		     name, ps->filename, name, name);
	  }
	if (!TYPE_IS_OPAQUE (SYMBOL_TYPE (sym)))
	  return SYMBOL_TYPE (sym);
      }
  }
  return (struct type *) 0;
}


/* Find the psymtab containing main(). */
/* FIXME:  What about languages without main() or specially linked
   executables that have no main() ? */

struct partial_symtab *
find_main_psymtab (void)
{
  struct partial_symtab *pst;
  struct objfile *objfile;

  ALL_PSYMTABS (objfile, pst)
  {
    if (lookup_partial_symbol (pst, main_name (), NULL, 1, VAR_DOMAIN))
      {
	return (pst);
      }
  }
  return (NULL);
}

/* Search BLOCK for symbol NAME in DOMAIN.

   Note that if NAME is the demangled form of a C++ symbol, we will fail
   to find a match during the binary search of the non-encoded names, but
   for now we don't worry about the slight inefficiency of looking for
   a match we'll never find, since it will go pretty quick.  Once the
   binary search terminates, we drop through and do a straight linear
   search on the symbols.  Each symbol which is marked as being a ObjC/C++
   symbol (language_cplus or language_objc set) has both the encoded and 
   non-encoded names tested for a match.

   If LINKAGE_NAME is non-NULL, verify that any symbol we find has this
   particular mangled name.
*/

struct symbol *
lookup_block_symbol (const struct block *block, const char *name,
		     const char *linkage_name,
		     const domain_enum domain)
{
  struct dict_iterator iter;
  struct symbol *sym;

  if (!BLOCK_FUNCTION (block))
    {
      for (sym = dict_iter_name_first (BLOCK_DICT (block), name, &iter);
	   sym != NULL;
	   sym = dict_iter_name_next (name, &iter))
	{
	  if (symbol_matches_domain (SYMBOL_LANGUAGE (sym),
				     SYMBOL_DOMAIN (sym), domain)
	      && (linkage_name != NULL
		  ? strcmp (SYMBOL_LINKAGE_NAME (sym), linkage_name) == 0 : 1))
	    return sym;
	}
      return NULL;
    }
  else
    {
      /* Note that parameter symbols do not always show up last in the
	 list; this loop makes sure to take anything else other than
	 parameter symbols first; it only uses parameter symbols as a
	 last resort.  Note that this only takes up extra computation
	 time on a match.  */

      struct symbol *sym_found = NULL;

      for (sym = dict_iter_name_first (BLOCK_DICT (block), name, &iter);
	   sym != NULL;
	   sym = dict_iter_name_next (name, &iter))
	{
	  if (symbol_matches_domain (SYMBOL_LANGUAGE (sym),
				     SYMBOL_DOMAIN (sym), domain)
	      && (linkage_name != NULL
		  ? strcmp (SYMBOL_LINKAGE_NAME (sym), linkage_name) == 0 : 1))
	    {
	      sym_found = sym;
	      if (!SYMBOL_IS_ARGUMENT (sym))
		{
		  break;
		}
	    }
	}
      return (sym_found);	/* Will be NULL if not found. */
    }
}

/* Find the symtab associated with PC and SECTION.  Look through the
   psymtabs and read in another symtab if necessary. */

struct symtab *
find_pc_sect_symtab (CORE_ADDR pc, struct obj_section *section)
{
  struct block *b;
  struct blockvector *bv;
  struct symtab *s = NULL;
  struct symtab *best_s = NULL;
  struct partial_symtab *ps;
  struct objfile *objfile;
  CORE_ADDR distance = 0;
  struct minimal_symbol *msymbol;

  /* If we know that this is not a text address, return failure.  This is
     necessary because we loop based on the block's high and low code
     addresses, which do not include the data ranges, and because
     we call find_pc_sect_psymtab which has a similar restriction based
     on the partial_symtab's texthigh and textlow.  */
  msymbol = lookup_minimal_symbol_by_pc_section (pc, section);
  if (msymbol
      && (msymbol->type == mst_data
	  || msymbol->type == mst_bss
	  || msymbol->type == mst_abs
	  || msymbol->type == mst_file_data
	  || msymbol->type == mst_file_bss))
    return NULL;

  /* Search all symtabs for the one whose file contains our address, and which
     is the smallest of all the ones containing the address.  This is designed
     to deal with a case like symtab a is at 0x1000-0x2000 and 0x3000-0x4000
     and symtab b is at 0x2000-0x3000.  So the GLOBAL_BLOCK for a is from
     0x1000-0x4000, but for address 0x2345 we want to return symtab b.

     This happens for native ecoff format, where code from included files
     gets its own symtab. The symtab for the included file should have
     been read in already via the dependency mechanism.
     It might be swifter to create several symtabs with the same name
     like xcoff does (I'm not sure).

     It also happens for objfiles that have their functions reordered.
     For these, the symtab we are looking for is not necessarily read in.  */

  ALL_PRIMARY_SYMTABS (objfile, s)
  {
    bv = BLOCKVECTOR (s);
    b = BLOCKVECTOR_BLOCK (bv, GLOBAL_BLOCK);

    if (BLOCK_START (b) <= pc
	&& BLOCK_END (b) > pc
	&& (distance == 0
	    || BLOCK_END (b) - BLOCK_START (b) < distance))
      {
	/* For an objfile that has its functions reordered,
	   find_pc_psymtab will find the proper partial symbol table
	   and we simply return its corresponding symtab.  */
	/* In order to better support objfiles that contain both
	   stabs and coff debugging info, we continue on if a psymtab
	   can't be found. */
	if ((objfile->flags & OBJF_REORDERED) && objfile->psymtabs)
	  {
	    ps = find_pc_sect_psymtab (pc, section);
	    if (ps)
	      return PSYMTAB_TO_SYMTAB (ps);
	  }
	if (section != 0)
	  {
	    struct dict_iterator iter;
	    struct symbol *sym = NULL;

	    ALL_BLOCK_SYMBOLS (b, iter, sym)
	      {
		fixup_symbol_section (sym, objfile);
		if (matching_obj_sections (SYMBOL_OBJ_SECTION (sym), section))
		  break;
	      }
	    if (sym == NULL)
	      continue;		/* no symbol in this symtab matches section */
	  }
	distance = BLOCK_END (b) - BLOCK_START (b);
	best_s = s;
      }
  }

  if (best_s != NULL)
    return (best_s);

  s = NULL;
  ps = find_pc_sect_psymtab (pc, section);
  if (ps)
    {
      if (ps->readin)
	/* Might want to error() here (in case symtab is corrupt and
	   will cause a core dump), but maybe we can successfully
	   continue, so let's not.  */
	warning (_("\
(Internal error: pc 0x%s in read in psymtab, but not in symtab.)\n"),
		 paddr_nz (pc));
      s = PSYMTAB_TO_SYMTAB (ps);
    }
  return (s);
}

/* Find the symtab associated with PC.  Look through the psymtabs and
   read in another symtab if necessary.  Backward compatibility, no section */

struct symtab *
find_pc_symtab (CORE_ADDR pc)
{
  return find_pc_sect_symtab (pc, find_pc_mapped_section (pc));
}


/* Find the source file and line number for a given PC value and SECTION.
   Return a structure containing a symtab pointer, a line number,
   and a pc range for the entire source line.
   The value's .pc field is NOT the specified pc.
   NOTCURRENT nonzero means, if specified pc is on a line boundary,
   use the line that ends there.  Otherwise, in that case, the line
   that begins there is used.  */

/* The big complication here is that a line may start in one file, and end just
   before the start of another file.  This usually occurs when you #include
   code in the middle of a subroutine.  To properly find the end of a line's PC
   range, we must search all symtabs associated with this compilation unit, and
   find the one whose first PC is closer than that of the next line in this
   symtab.  */

/* If it's worth the effort, we could be using a binary search.  */

struct symtab_and_line
find_pc_sect_line (CORE_ADDR pc, struct obj_section *section, int notcurrent)
{
  struct symtab *s;
  struct linetable *l;
  int len;
  int i;
  struct linetable_entry *item;
  struct symtab_and_line val;
  struct blockvector *bv;
  struct minimal_symbol *msymbol;
  struct minimal_symbol *mfunsym;

  /* Info on best line seen so far, and where it starts, and its file.  */

  struct linetable_entry *best = NULL;
  CORE_ADDR best_end = 0;
  struct symtab *best_symtab = 0;

  /* Store here the first line number
     of a file which contains the line at the smallest pc after PC.
     If we don't find a line whose range contains PC,
     we will use a line one less than this,
     with a range from the start of that file to the first line's pc.  */
  struct linetable_entry *alt = NULL;
  struct symtab *alt_symtab = 0;

  /* Info on best line seen in this file.  */

  struct linetable_entry *prev;

  /* If this pc is not from the current frame,
     it is the address of the end of a call instruction.
     Quite likely that is the start of the following statement.
     But what we want is the statement containing the instruction.
     Fudge the pc to make sure we get that.  */

  init_sal (&val);		/* initialize to zeroes */

  /* It's tempting to assume that, if we can't find debugging info for
     any function enclosing PC, that we shouldn't search for line
     number info, either.  However, GAS can emit line number info for
     assembly files --- very helpful when debugging hand-written
     assembly code.  In such a case, we'd have no debug info for the
     function, but we would have line info.  */

  if (notcurrent)
    pc -= 1;

  /* elz: added this because this function returned the wrong
     information if the pc belongs to a stub (import/export)
     to call a shlib function. This stub would be anywhere between
     two functions in the target, and the line info was erroneously 
     taken to be the one of the line before the pc. 
   */
  /* RT: Further explanation:

   * We have stubs (trampolines) inserted between procedures.
   *
   * Example: "shr1" exists in a shared library, and a "shr1" stub also
   * exists in the main image.
   *
   * In the minimal symbol table, we have a bunch of symbols
   * sorted by start address. The stubs are marked as "trampoline",
   * the others appear as text. E.g.:
   *
   *  Minimal symbol table for main image 
   *     main:  code for main (text symbol)
   *     shr1: stub  (trampoline symbol)
   *     foo:   code for foo (text symbol)
   *     ...
   *  Minimal symbol table for "shr1" image:
   *     ...
   *     shr1: code for shr1 (text symbol)
   *     ...
   *
   * So the code below is trying to detect if we are in the stub
   * ("shr1" stub), and if so, find the real code ("shr1" trampoline),
   * and if found,  do the symbolization from the real-code address
   * rather than the stub address.
   *
   * Assumptions being made about the minimal symbol table:
   *   1. lookup_minimal_symbol_by_pc() will return a trampoline only
   *      if we're really in the trampoline. If we're beyond it (say
   *      we're in "foo" in the above example), it'll have a closer 
   *      symbol (the "foo" text symbol for example) and will not
   *      return the trampoline.
   *   2. lookup_minimal_symbol_text() will find a real text symbol
   *      corresponding to the trampoline, and whose address will
   *      be different than the trampoline address. I put in a sanity
   *      check for the address being the same, to avoid an
   *      infinite recursion.
   */
  msymbol = lookup_minimal_symbol_by_pc (pc);
  if (msymbol != NULL)
    if (MSYMBOL_TYPE (msymbol) == mst_solib_trampoline)
      {
	mfunsym = lookup_minimal_symbol_text (SYMBOL_LINKAGE_NAME (msymbol),
					      NULL);
	if (mfunsym == NULL)
	  /* I eliminated this warning since it is coming out
	   * in the following situation:
	   * gdb shmain // test program with shared libraries
	   * (gdb) break shr1  // function in shared lib
	   * Warning: In stub for ...
	   * In the above situation, the shared lib is not loaded yet, 
	   * so of course we can't find the real func/line info,
	   * but the "break" still works, and the warning is annoying.
	   * So I commented out the warning. RT */
	  /* warning ("In stub for %s; unable to find real function/line info", SYMBOL_LINKAGE_NAME (msymbol)) */ ;
	/* fall through */
	else if (SYMBOL_VALUE_ADDRESS (mfunsym) == SYMBOL_VALUE_ADDRESS (msymbol))
	  /* Avoid infinite recursion */
	  /* See above comment about why warning is commented out */
	  /* warning ("In stub for %s; unable to find real function/line info", SYMBOL_LINKAGE_NAME (msymbol)) */ ;
	/* fall through */
	else
	  return find_pc_line (SYMBOL_VALUE_ADDRESS (mfunsym), 0);
      }


  s = find_pc_sect_symtab (pc, section);
  if (!s)
    {
      /* if no symbol information, return previous pc */
      if (notcurrent)
	pc++;
      val.pc = pc;
      return val;
    }

  bv = BLOCKVECTOR (s);

  /* Look at all the symtabs that share this blockvector.
     They all have the same apriori range, that we found was right;
     but they have different line tables.  */

  for (; s && BLOCKVECTOR (s) == bv; s = s->next)
    {
      /* Find the best line in this symtab.  */
      l = LINETABLE (s);
      if (!l)
	continue;
      len = l->nitems;
      if (len <= 0)
	{
	  /* I think len can be zero if the symtab lacks line numbers
	     (e.g. gcc -g1).  (Either that or the LINETABLE is NULL;
	     I'm not sure which, and maybe it depends on the symbol
	     reader).  */
	  continue;
	}

      prev = NULL;
      item = l->item;		/* Get first line info */

      /* Is this file's first line closer than the first lines of other files?
         If so, record this file, and its first line, as best alternate.  */
      if (item->pc > pc && (!alt || item->pc < alt->pc))
	{
	  alt = item;
	  alt_symtab = s;
	}

      for (i = 0; i < len; i++, item++)
	{
	  /* Leave prev pointing to the linetable entry for the last line
	     that started at or before PC.  */
	  if (item->pc > pc)
	    break;

	  prev = item;
	}

      /* At this point, prev points at the line whose start addr is <= pc, and
         item points at the next line.  If we ran off the end of the linetable
         (pc >= start of the last line), then prev == item.  If pc < start of
         the first line, prev will not be set.  */

      /* Is this file's best line closer than the best in the other files?
         If so, record this file, and its best line, as best so far.  Don't
         save prev if it represents the end of a function (i.e. line number
         0) instead of a real line.  */

      if (prev && prev->line && (!best || prev->pc > best->pc))
	{
	  best = prev;
	  best_symtab = s;

	  /* Discard BEST_END if it's before the PC of the current BEST.  */
	  if (best_end <= best->pc)
	    best_end = 0;
	}

      /* If another line (denoted by ITEM) is in the linetable and its
         PC is after BEST's PC, but before the current BEST_END, then
	 use ITEM's PC as the new best_end.  */
      if (best && i < len && item->pc > best->pc
          && (best_end == 0 || best_end > item->pc))
	best_end = item->pc;
    }

  if (!best_symtab)
    {
      /* If we didn't find any line number info, just return zeros.
	 We used to return alt->line - 1 here, but that could be
	 anywhere; if we don't have line number info for this PC,
	 don't make some up.  */
      val.pc = pc;
    }
  else if (best->line == 0)
    {
      /* If our best fit is in a range of PC's for which no line
	 number info is available (line number is zero) then we didn't
	 find any valid line information. */
      val.pc = pc;
    }
  else
    {
      val.symtab = best_symtab;
      val.line = best->line;
      val.pc = best->pc;
      if (best_end && (!alt || best_end < alt->pc))
	val.end = best_end;
      else if (alt)
	val.end = alt->pc;
      else
	val.end = BLOCK_END (BLOCKVECTOR_BLOCK (bv, GLOBAL_BLOCK));
    }
  val.section = section;
  return val;
}

/* Backward compatibility (no section) */

struct symtab_and_line
find_pc_line (CORE_ADDR pc, int notcurrent)
{
  struct obj_section *section;

  section = find_pc_overlay (pc);
  if (pc_in_unmapped_range (pc, section))
    pc = overlay_mapped_address (pc, section);
  return find_pc_sect_line (pc, section, notcurrent);
}

/* Find line number LINE in any symtab whose name is the same as
   SYMTAB.

   If found, return the symtab that contains the linetable in which it was
   found, set *INDEX to the index in the linetable of the best entry
   found, and set *EXACT_MATCH nonzero if the value returned is an
   exact match.

   If not found, return NULL.  */

struct symtab *
find_line_symtab (struct symtab *symtab, int line, int *index, int *exact_match)
{
  int exact;

  /* BEST_INDEX and BEST_LINETABLE identify the smallest linenumber > LINE
     so far seen.  */

  int best_index;
  struct linetable *best_linetable;
  struct symtab *best_symtab;

  /* First try looking it up in the given symtab.  */
  best_linetable = LINETABLE (symtab);
  best_symtab = symtab;
  best_index = find_line_common (best_linetable, line, &exact);
  if (best_index < 0 || !exact)
    {
      /* Didn't find an exact match.  So we better keep looking for
         another symtab with the same name.  In the case of xcoff,
         multiple csects for one source file (produced by IBM's FORTRAN
         compiler) produce multiple symtabs (this is unavoidable
         assuming csects can be at arbitrary places in memory and that
         the GLOBAL_BLOCK of a symtab has a begin and end address).  */

      /* BEST is the smallest linenumber > LINE so far seen,
         or 0 if none has been seen so far.
         BEST_INDEX and BEST_LINETABLE identify the item for it.  */
      int best;

      struct objfile *objfile;
      struct symtab *s;
      struct partial_symtab *p;

      if (best_index >= 0)
	best = best_linetable->item[best_index].line;
      else
	best = 0;

      ALL_PSYMTABS (objfile, p)
      {
        if (strcmp (symtab->filename, p->filename) != 0)
          continue;
        PSYMTAB_TO_SYMTAB (p);
      }

      ALL_SYMTABS (objfile, s)
      {
	struct linetable *l;
	int ind;

	if (strcmp (symtab->filename, s->filename) != 0)
	  continue;
	l = LINETABLE (s);
	ind = find_line_common (l, line, &exact);
	if (ind >= 0)
	  {
	    if (exact)
	      {
		best_index = ind;
		best_linetable = l;
		best_symtab = s;
		goto done;
	      }
	    if (best == 0 || l->item[ind].line < best)
	      {
		best = l->item[ind].line;
		best_index = ind;
		best_linetable = l;
		best_symtab = s;
	      }
	  }
      }
    }
done:
  if (best_index < 0)
    return NULL;

  if (index)
    *index = best_index;
  if (exact_match)
    *exact_match = exact;

  return best_symtab;
}

/* Set the PC value for a given source file and line number and return true.
   Returns zero for invalid line number (and sets the PC to 0).
   The source file is specified with a struct symtab.  */

int
find_line_pc (struct symtab *symtab, int line, CORE_ADDR *pc)
{
  struct linetable *l;
  int ind;

  *pc = 0;
  if (symtab == 0)
    return 0;

  symtab = find_line_symtab (symtab, line, &ind, NULL);
  if (symtab != NULL)
    {
      l = LINETABLE (symtab);
      *pc = l->item[ind].pc;
      return 1;
    }
  else
    return 0;
}

/* Find the range of pc values in a line.
   Store the starting pc of the line into *STARTPTR
   and the ending pc (start of next line) into *ENDPTR.
   Returns 1 to indicate success.
   Returns 0 if could not find the specified line.  */

int
find_line_pc_range (struct symtab_and_line sal, CORE_ADDR *startptr,
		    CORE_ADDR *endptr)
{
  CORE_ADDR startaddr;
  struct symtab_and_line found_sal;

  startaddr = sal.pc;
  if (startaddr == 0 && !find_line_pc (sal.symtab, sal.line, &startaddr))
    return 0;

  /* This whole function is based on address.  For example, if line 10 has
     two parts, one from 0x100 to 0x200 and one from 0x300 to 0x400, then
     "info line *0x123" should say the line goes from 0x100 to 0x200
     and "info line *0x355" should say the line goes from 0x300 to 0x400.
     This also insures that we never give a range like "starts at 0x134
     and ends at 0x12c".  */

  found_sal = find_pc_sect_line (startaddr, sal.section, 0);
  if (found_sal.line != sal.line)
    {
      /* The specified line (sal) has zero bytes.  */
      *startptr = found_sal.pc;
      *endptr = found_sal.pc;
    }
  else
    {
      *startptr = found_sal.pc;
      *endptr = found_sal.end;
    }
  return 1;
}

/* Given a line table and a line number, return the index into the line
   table for the pc of the nearest line whose number is >= the specified one.
   Return -1 if none is found.  The value is >= 0 if it is an index.

   Set *EXACT_MATCH nonzero if the value returned is an exact match.  */

static int
find_line_common (struct linetable *l, int lineno,
		  int *exact_match)
{
  int i;
  int len;

  /* BEST is the smallest linenumber > LINENO so far seen,
     or 0 if none has been seen so far.
     BEST_INDEX identifies the item for it.  */

  int best_index = -1;
  int best = 0;

  *exact_match = 0;

  if (lineno <= 0)
    return -1;
  if (l == 0)
    return -1;

  len = l->nitems;
  for (i = 0; i < len; i++)
    {
      struct linetable_entry *item = &(l->item[i]);

      if (item->line == lineno)
	{
	  /* Return the first (lowest address) entry which matches.  */
	  *exact_match = 1;
	  return i;
	}

      if (item->line > lineno && (best == 0 || item->line < best))
	{
	  best = item->line;
	  best_index = i;
	}
    }

  /* If we got here, we didn't get an exact match.  */
  return best_index;
}

int
find_pc_line_pc_range (CORE_ADDR pc, CORE_ADDR *startptr, CORE_ADDR *endptr)
{
  struct symtab_and_line sal;
  sal = find_pc_line (pc, 0);
  *startptr = sal.pc;
  *endptr = sal.end;
  return sal.symtab != 0;
}

/* Given a function start address PC and SECTION, find the first
   address after the function prologue.  */
CORE_ADDR
find_function_start_pc (struct gdbarch *gdbarch,
			CORE_ADDR pc, struct obj_section *section)
{
  /* If the function is in an unmapped overlay, use its unmapped LMA address,
     so that gdbarch_skip_prologue has something unique to work on.  */
  if (section_is_overlay (section) && !section_is_mapped (section))
    pc = overlay_unmapped_address (pc, section);

  pc += gdbarch_deprecated_function_start_offset (gdbarch);
  pc = gdbarch_skip_prologue (gdbarch, pc);

  /* For overlays, map pc back into its mapped VMA range.  */
  pc = overlay_mapped_address (pc, section);

  return pc;
}

/* Given a function symbol SYM, find the symtab and line for the start
   of the function.
   If the argument FUNFIRSTLINE is nonzero, we want the first line
   of real code inside the function.  */

struct symtab_and_line
find_function_start_sal (struct symbol *sym, int funfirstline)
{
  struct block *block = SYMBOL_BLOCK_VALUE (sym);
  struct objfile *objfile = lookup_objfile_from_block (block);
  struct gdbarch *gdbarch = get_objfile_arch (objfile);

  CORE_ADDR pc;
  struct symtab_and_line sal;

  pc = BLOCK_START (block);
  fixup_symbol_section (sym, objfile);
  if (funfirstline)
    {
      /* Skip "first line" of function (which is actually its prologue).  */
      pc = find_function_start_pc (gdbarch, pc, SYMBOL_OBJ_SECTION (sym));
    }
  sal = find_pc_sect_line (pc, SYMBOL_OBJ_SECTION (sym), 0);

  /* Check if gdbarch_skip_prologue left us in mid-line, and the next
     line is still part of the same function.  */
  if (sal.pc != pc
      && BLOCK_START (block) <= sal.end
      && sal.end < BLOCK_END (block))
    {
      /* First pc of next line */
      pc = sal.end;
      /* Recalculate the line number (might not be N+1).  */
      sal = find_pc_sect_line (pc, SYMBOL_OBJ_SECTION (sym), 0);
    }

  /* On targets with executable formats that don't have a concept of
     constructors (ELF with .init has, PE doesn't), gcc emits a call
     to `__main' in `main' between the prologue and before user
     code.  */
  if (funfirstline
      && gdbarch_skip_main_prologue_p (current_gdbarch)
      && SYMBOL_LINKAGE_NAME (sym)
      && strcmp (SYMBOL_LINKAGE_NAME (sym), "main") == 0)
    {
      pc = gdbarch_skip_main_prologue (current_gdbarch, pc);
      /* Recalculate the line number (might not be N+1).  */
      sal = find_pc_sect_line (pc, SYMBOL_OBJ_SECTION (sym), 0);
    }

  sal.pc = pc;

  return sal;
}

/* If P is of the form "operator[ \t]+..." where `...' is
   some legitimate operator text, return a pointer to the
   beginning of the substring of the operator text.
   Otherwise, return "".  */
char *
operator_chars (char *p, char **end)
{
  *end = "";
  if (strncmp (p, "operator", 8))
    return *end;
  p += 8;

  /* Don't get faked out by `operator' being part of a longer
     identifier.  */
  if (isalpha (*p) || *p == '_' || *p == '$' || *p == '\0')
    return *end;

  /* Allow some whitespace between `operator' and the operator symbol.  */
  while (*p == ' ' || *p == '\t')
    p++;

  /* Recognize 'operator TYPENAME'. */

  if (isalpha (*p) || *p == '_' || *p == '$')
    {
      char *q = p + 1;
      while (isalnum (*q) || *q == '_' || *q == '$')
	q++;
      *end = q;
      return p;
    }

  while (*p)
    switch (*p)
      {
      case '\\':			/* regexp quoting */
	if (p[1] == '*')
	  {
	    if (p[2] == '=')	/* 'operator\*=' */
	      *end = p + 3;
	    else			/* 'operator\*'  */
	      *end = p + 2;
	    return p;
	  }
	else if (p[1] == '[')
	  {
	    if (p[2] == ']')
	      error (_("mismatched quoting on brackets, try 'operator\\[\\]'"));
	    else if (p[2] == '\\' && p[3] == ']')
	      {
		*end = p + 4;	/* 'operator\[\]' */
		return p;
	      }
	    else
	      error (_("nothing is allowed between '[' and ']'"));
	  }
	else 
	  {
	    /* Gratuitous qoute: skip it and move on. */
	    p++;
	    continue;
	  }
	break;
      case '!':
      case '=':
      case '*':
      case '/':
      case '%':
      case '^':
	if (p[1] == '=')
	  *end = p + 2;
	else
	  *end = p + 1;
	return p;
      case '<':
      case '>':
      case '+':
      case '-':
      case '&':
      case '|':
	if (p[0] == '-' && p[1] == '>')
	  {
	    /* Struct pointer member operator 'operator->'. */
	    if (p[2] == '*')
	      {
		*end = p + 3;	/* 'operator->*' */
		return p;
	      }
	    else if (p[2] == '\\')
	      {
		*end = p + 4;	/* Hopefully 'operator->\*' */
		return p;
	      }
	    else
	      {
		*end = p + 2;	/* 'operator->' */
		return p;
	      }
	  }
	if (p[1] == '=' || p[1] == p[0])
	  *end = p + 2;
	else
	  *end = p + 1;
	return p;
      case '~':
      case ',':
	*end = p + 1;
	return p;
      case '(':
	if (p[1] != ')')
	  error (_("`operator ()' must be specified without whitespace in `()'"));
	*end = p + 2;
	return p;
      case '?':
	if (p[1] != ':')
	  error (_("`operator ?:' must be specified without whitespace in `?:'"));
	*end = p + 2;
	return p;
      case '[':
	if (p[1] != ']')
	  error (_("`operator []' must be specified without whitespace in `[]'"));
	*end = p + 2;
	return p;
      default:
	error (_("`operator %s' not supported"), p);
	break;
      }

  *end = "";
  return *end;
}


/* If FILE is not already in the table of files, return zero;
   otherwise return non-zero.  Optionally add FILE to the table if ADD
   is non-zero.  If *FIRST is non-zero, forget the old table
   contents.  */
static int
filename_seen (const char *file, int add, int *first)
{
  /* Table of files seen so far.  */
  static const char **tab = NULL;
  /* Allocated size of tab in elements.
     Start with one 256-byte block (when using GNU malloc.c).
     24 is the malloc overhead when range checking is in effect.  */
  static int tab_alloc_size = (256 - 24) / sizeof (char *);
  /* Current size of tab in elements.  */
  static int tab_cur_size;
  const char **p;

  if (*first)
    {
      if (tab == NULL)
	tab = (const char **) xmalloc (tab_alloc_size * sizeof (*tab));
      tab_cur_size = 0;
    }

  /* Is FILE in tab?  */
  for (p = tab; p < tab + tab_cur_size; p++)
    if (strcmp (*p, file) == 0)
      return 1;

  /* No; maybe add it to tab.  */
  if (add)
    {
      if (tab_cur_size == tab_alloc_size)
	{
	  tab_alloc_size *= 2;
	  tab = (const char **) xrealloc ((char *) tab,
					  tab_alloc_size * sizeof (*tab));
	}
      tab[tab_cur_size++] = file;
    }

  return 0;
}

/* Slave routine for sources_info.  Force line breaks at ,'s.
   NAME is the name to print and *FIRST is nonzero if this is the first
   name printed.  Set *FIRST to zero.  */
static void
output_source_filename (const char *name, int *first)
{
  /* Since a single source file can result in several partial symbol
     tables, we need to avoid printing it more than once.  Note: if
     some of the psymtabs are read in and some are not, it gets
     printed both under "Source files for which symbols have been
     read" and "Source files for which symbols will be read in on
     demand".  I consider this a reasonable way to deal with the
     situation.  I'm not sure whether this can also happen for
     symtabs; it doesn't hurt to check.  */

  /* Was NAME already seen?  */
  if (filename_seen (name, 1, first))
    {
      /* Yes; don't print it again.  */
      return;
    }
  /* No; print it and reset *FIRST.  */
  if (*first)
    {
      *first = 0;
    }
  else
    {
      printf_filtered (", ");
    }

  wrap_here ("");
  fputs_filtered (name, gdb_stdout);
}

static void
sources_info (char *ignore, int from_tty)
{
  struct symtab *s;
  struct partial_symtab *ps;
  struct objfile *objfile;
  int first;

  if (!have_full_symbols () && !have_partial_symbols ())
    {
      error (_("No symbol table is loaded.  Use the \"file\" command."));
    }

  printf_filtered ("Source files for which symbols have been read in:\n\n");

  first = 1;
  ALL_SYMTABS (objfile, s)
  {
    const char *fullname = symtab_to_fullname (s);
    output_source_filename (fullname ? fullname : s->filename, &first);
  }
  printf_filtered ("\n\n");

  printf_filtered ("Source files for which symbols will be read in on demand:\n\n");

  first = 1;
  ALL_PSYMTABS (objfile, ps)
  {
    if (!ps->readin)
      {
	const char *fullname = psymtab_to_fullname (ps);
	output_source_filename (fullname ? fullname : ps->filename, &first);
      }
  }
  printf_filtered ("\n");
}

static int
file_matches (char *file, char *files[], int nfiles)
{
  int i;

  if (file != NULL && nfiles != 0)
    {
      for (i = 0; i < nfiles; i++)
	{
	  if (strcmp (files[i], lbasename (file)) == 0)
	    return 1;
	}
    }
  else if (nfiles == 0)
    return 1;
  return 0;
}

/* Free any memory associated with a search. */
void
free_search_symbols (struct symbol_search *symbols)
{
  struct symbol_search *p;
  struct symbol_search *next;

  for (p = symbols; p != NULL; p = next)
    {
      next = p->next;
      xfree (p);
    }
}

static void
do_free_search_symbols_cleanup (void *symbols)
{
  free_search_symbols (symbols);
}

struct cleanup *
make_cleanup_free_search_symbols (struct symbol_search *symbols)
{
  return make_cleanup (do_free_search_symbols_cleanup, symbols);
}

/* Helper function for sort_search_symbols and qsort.  Can only
   sort symbols, not minimal symbols.  */
static int
compare_search_syms (const void *sa, const void *sb)
{
  struct symbol_search **sym_a = (struct symbol_search **) sa;
  struct symbol_search **sym_b = (struct symbol_search **) sb;

  return strcmp (SYMBOL_PRINT_NAME ((*sym_a)->symbol),
		 SYMBOL_PRINT_NAME ((*sym_b)->symbol));
}

/* Sort the ``nfound'' symbols in the list after prevtail.  Leave
   prevtail where it is, but update its next pointer to point to
   the first of the sorted symbols.  */
static struct symbol_search *
sort_search_symbols (struct symbol_search *prevtail, int nfound)
{
  struct symbol_search **symbols, *symp, *old_next;
  int i;

  symbols = (struct symbol_search **) xmalloc (sizeof (struct symbol_search *)
					       * nfound);
  symp = prevtail->next;
  for (i = 0; i < nfound; i++)
    {
      symbols[i] = symp;
      symp = symp->next;
    }
  /* Generally NULL.  */
  old_next = symp;

  qsort (symbols, nfound, sizeof (struct symbol_search *),
	 compare_search_syms);

  symp = prevtail;
  for (i = 0; i < nfound; i++)
    {
      symp->next = symbols[i];
      symp = symp->next;
    }
  symp->next = old_next;

  xfree (symbols);
  return symp;
}

/* Search the symbol table for matches to the regular expression REGEXP,
   returning the results in *MATCHES.

   Only symbols of KIND are searched:
   FUNCTIONS_DOMAIN - search all functions
   TYPES_DOMAIN     - search all type names
   METHODS_DOMAIN   - search all methods NOT IMPLEMENTED
   VARIABLES_DOMAIN - search all symbols, excluding functions, type names,
   and constants (enums)

   free_search_symbols should be called when *MATCHES is no longer needed.

   The results are sorted locally; each symtab's global and static blocks are
   separately alphabetized.
 */
void
search_symbols (char *regexp, domain_enum kind, int nfiles, char *files[],
		struct symbol_search **matches)
{
  struct symtab *s;
  struct partial_symtab *ps;
  struct blockvector *bv;
  struct block *b;
  int i = 0;
  struct dict_iterator iter;
  struct symbol *sym;
  struct partial_symbol **psym;
  struct objfile *objfile;
  struct minimal_symbol *msymbol;
  char *val;
  int found_misc = 0;
  static enum minimal_symbol_type types[]
  =
  {mst_data, mst_text, mst_abs, mst_unknown};
  static enum minimal_symbol_type types2[]
  =
  {mst_bss, mst_file_text, mst_abs, mst_unknown};
  static enum minimal_symbol_type types3[]
  =
  {mst_file_data, mst_solib_trampoline, mst_abs, mst_unknown};
  static enum minimal_symbol_type types4[]
  =
  {mst_file_bss, mst_text, mst_abs, mst_unknown};
  enum minimal_symbol_type ourtype;
  enum minimal_symbol_type ourtype2;
  enum minimal_symbol_type ourtype3;
  enum minimal_symbol_type ourtype4;
  struct symbol_search *sr;
  struct symbol_search *psr;
  struct symbol_search *tail;
  struct cleanup *old_chain = NULL;

  if (kind < VARIABLES_DOMAIN)
    error (_("must search on specific domain"));

  ourtype = types[(int) (kind - VARIABLES_DOMAIN)];
  ourtype2 = types2[(int) (kind - VARIABLES_DOMAIN)];
  ourtype3 = types3[(int) (kind - VARIABLES_DOMAIN)];
  ourtype4 = types4[(int) (kind - VARIABLES_DOMAIN)];

  sr = *matches = NULL;
  tail = NULL;

  if (regexp != NULL)
    {
      /* Make sure spacing is right for C++ operators.
         This is just a courtesy to make the matching less sensitive
         to how many spaces the user leaves between 'operator'
         and <TYPENAME> or <OPERATOR>. */
      char *opend;
      char *opname = operator_chars (regexp, &opend);
      if (*opname)
	{
	  int fix = -1;		/* -1 means ok; otherwise number of spaces needed. */
	  if (isalpha (*opname) || *opname == '_' || *opname == '$')
	    {
	      /* There should 1 space between 'operator' and 'TYPENAME'. */
	      if (opname[-1] != ' ' || opname[-2] == ' ')
		fix = 1;
	    }
	  else
	    {
	      /* There should 0 spaces between 'operator' and 'OPERATOR'. */
	      if (opname[-1] == ' ')
		fix = 0;
	    }
	  /* If wrong number of spaces, fix it. */
	  if (fix >= 0)
	    {
	      char *tmp = (char *) alloca (8 + fix + strlen (opname) + 1);
	      sprintf (tmp, "operator%.*s%s", fix, " ", opname);
	      regexp = tmp;
	    }
	}

      if (0 != (val = re_comp (regexp)))
	error (_("Invalid regexp (%s): %s"), val, regexp);
    }

  /* Search through the partial symtabs *first* for all symbols
     matching the regexp.  That way we don't have to reproduce all of
     the machinery below. */

  ALL_PSYMTABS (objfile, ps)
  {
    struct partial_symbol **bound, **gbound, **sbound;
    int keep_going = 1;

    if (ps->readin)
      continue;

    gbound = objfile->global_psymbols.list + ps->globals_offset + ps->n_global_syms;
    sbound = objfile->static_psymbols.list + ps->statics_offset + ps->n_static_syms;
    bound = gbound;

    /* Go through all of the symbols stored in a partial
       symtab in one loop. */
    psym = objfile->global_psymbols.list + ps->globals_offset;
    while (keep_going)
      {
	if (psym >= bound)
	  {
	    if (bound == gbound && ps->n_static_syms != 0)
	      {
		psym = objfile->static_psymbols.list + ps->statics_offset;
		bound = sbound;
	      }
	    else
	      keep_going = 0;
	    continue;
	  }
	else
	  {
	    QUIT;

	    /* If it would match (logic taken from loop below)
	       load the file and go on to the next one.  We check the
	       filename here, but that's a bit bogus: we don't know
	       what file it really comes from until we have full
	       symtabs.  The symbol might be in a header file included by
	       this psymtab.  This only affects Insight.  */
	    if (file_matches (ps->filename, files, nfiles)
		&& ((regexp == NULL
		     || re_exec (SYMBOL_NATURAL_NAME (*psym)) != 0)
		    && ((kind == VARIABLES_DOMAIN && SYMBOL_CLASS (*psym) != LOC_TYPEDEF
			 && SYMBOL_CLASS (*psym) != LOC_BLOCK)
			|| (kind == FUNCTIONS_DOMAIN && SYMBOL_CLASS (*psym) == LOC_BLOCK)
			|| (kind == TYPES_DOMAIN && SYMBOL_CLASS (*psym) == LOC_TYPEDEF)
			|| (kind == METHODS_DOMAIN && SYMBOL_CLASS (*psym) == LOC_BLOCK))))
	      {
		PSYMTAB_TO_SYMTAB (ps);
		keep_going = 0;
	      }
	  }
	psym++;
      }
  }

  /* Here, we search through the minimal symbol tables for functions
     and variables that match, and force their symbols to be read.
     This is in particular necessary for demangled variable names,
     which are no longer put into the partial symbol tables.
     The symbol will then be found during the scan of symtabs below.

     For functions, find_pc_symtab should succeed if we have debug info
     for the function, for variables we have to call lookup_symbol
     to determine if the variable has debug info.
     If the lookup fails, set found_misc so that we will rescan to print
     any matching symbols without debug info.
   */

  if (nfiles == 0 && (kind == VARIABLES_DOMAIN || kind == FUNCTIONS_DOMAIN))
    {
      ALL_MSYMBOLS (objfile, msymbol)
      {
	if (MSYMBOL_TYPE (msymbol) == ourtype ||
	    MSYMBOL_TYPE (msymbol) == ourtype2 ||
	    MSYMBOL_TYPE (msymbol) == ourtype3 ||
	    MSYMBOL_TYPE (msymbol) == ourtype4)
	  {
	    if (regexp == NULL
		|| re_exec (SYMBOL_NATURAL_NAME (msymbol)) != 0)
	      {
		if (0 == find_pc_symtab (SYMBOL_VALUE_ADDRESS (msymbol)))
		  {
		    /* FIXME: carlton/2003-02-04: Given that the
		       semantics of lookup_symbol keeps on changing
		       slightly, it would be a nice idea if we had a
		       function lookup_symbol_minsym that found the
		       symbol associated to a given minimal symbol (if
		       any).  */
		    if (kind == FUNCTIONS_DOMAIN
			|| lookup_symbol (SYMBOL_LINKAGE_NAME (msymbol),
					  (struct block *) NULL,
					  VAR_DOMAIN, 0)
			== NULL)
		      found_misc = 1;
		  }
	      }
	  }
      }
    }

  ALL_PRIMARY_SYMTABS (objfile, s)
  {
    bv = BLOCKVECTOR (s);
      for (i = GLOBAL_BLOCK; i <= STATIC_BLOCK; i++)
	{
	  struct symbol_search *prevtail = tail;
	  int nfound = 0;
	  b = BLOCKVECTOR_BLOCK (bv, i);
	  ALL_BLOCK_SYMBOLS (b, iter, sym)
	    {
	      struct symtab *real_symtab = SYMBOL_SYMTAB (sym);
	      QUIT;

	      if (file_matches (real_symtab->filename, files, nfiles)
		  && ((regexp == NULL
		       || re_exec (SYMBOL_NATURAL_NAME (sym)) != 0)
		      && ((kind == VARIABLES_DOMAIN && SYMBOL_CLASS (sym) != LOC_TYPEDEF
			   && SYMBOL_CLASS (sym) != LOC_BLOCK
			   && SYMBOL_CLASS (sym) != LOC_CONST)
			  || (kind == FUNCTIONS_DOMAIN && SYMBOL_CLASS (sym) == LOC_BLOCK)
			  || (kind == TYPES_DOMAIN && SYMBOL_CLASS (sym) == LOC_TYPEDEF)
			  || (kind == METHODS_DOMAIN && SYMBOL_CLASS (sym) == LOC_BLOCK))))
		{
		  /* match */
		  psr = (struct symbol_search *) xmalloc (sizeof (struct symbol_search));
		  psr->block = i;
		  psr->symtab = real_symtab;
		  psr->symbol = sym;
		  psr->msymbol = NULL;
		  psr->next = NULL;
		  if (tail == NULL)
		    sr = psr;
		  else
		    tail->next = psr;
		  tail = psr;
		  nfound ++;
		}
	    }
	  if (nfound > 0)
	    {
	      if (prevtail == NULL)
		{
		  struct symbol_search dummy;

		  dummy.next = sr;
		  tail = sort_search_symbols (&dummy, nfound);
		  sr = dummy.next;

		  old_chain = make_cleanup_free_search_symbols (sr);
		}
	      else
		tail = sort_search_symbols (prevtail, nfound);
	    }
	}
  }

  /* If there are no eyes, avoid all contact.  I mean, if there are
     no debug symbols, then print directly from the msymbol_vector.  */

  if (found_misc || kind != FUNCTIONS_DOMAIN)
    {
      ALL_MSYMBOLS (objfile, msymbol)
      {
	if (MSYMBOL_TYPE (msymbol) == ourtype ||
	    MSYMBOL_TYPE (msymbol) == ourtype2 ||
	    MSYMBOL_TYPE (msymbol) == ourtype3 ||
	    MSYMBOL_TYPE (msymbol) == ourtype4)
	  {
	    if (regexp == NULL
		|| re_exec (SYMBOL_NATURAL_NAME (msymbol)) != 0)
	      {
		/* Functions:  Look up by address. */
		if (kind != FUNCTIONS_DOMAIN ||
		    (0 == find_pc_symtab (SYMBOL_VALUE_ADDRESS (msymbol))))
		  {
		    /* Variables/Absolutes:  Look up by name */
		    if (lookup_symbol (SYMBOL_LINKAGE_NAME (msymbol),
				       (struct block *) NULL, VAR_DOMAIN, 0)
			 == NULL)
		      {
			/* match */
			psr = (struct symbol_search *) xmalloc (sizeof (struct symbol_search));
			psr->block = i;
			psr->msymbol = msymbol;
			psr->symtab = NULL;
			psr->symbol = NULL;
			psr->next = NULL;
			if (tail == NULL)
			  {
			    sr = psr;
			    old_chain = make_cleanup_free_search_symbols (sr);
			  }
			else
			  tail->next = psr;
			tail = psr;
		      }
		  }
	      }
	  }
      }
    }

  *matches = sr;
  if (sr != NULL)
    discard_cleanups (old_chain);
}

/* Helper function for symtab_symbol_info, this function uses
   the data returned from search_symbols() to print information
   regarding the match to gdb_stdout.
 */
static void
print_symbol_info (domain_enum kind, struct symtab *s, struct symbol *sym,
		   int block, char *last)
{
  if (last == NULL || strcmp (last, s->filename) != 0)
    {
      fputs_filtered ("\nFile ", gdb_stdout);
      fputs_filtered (s->filename, gdb_stdout);
      fputs_filtered (":\n", gdb_stdout);
    }

  if (kind != TYPES_DOMAIN && block == STATIC_BLOCK)
    printf_filtered ("static ");

  /* Typedef that is not a C++ class */
  if (kind == TYPES_DOMAIN
      && SYMBOL_DOMAIN (sym) != STRUCT_DOMAIN)
    typedef_print (SYMBOL_TYPE (sym), sym, gdb_stdout);
  /* variable, func, or typedef-that-is-c++-class */
  else if (kind < TYPES_DOMAIN ||
	   (kind == TYPES_DOMAIN &&
	    SYMBOL_DOMAIN (sym) == STRUCT_DOMAIN))
    {
      type_print (SYMBOL_TYPE (sym),
		  (SYMBOL_CLASS (sym) == LOC_TYPEDEF
		   ? "" : SYMBOL_PRINT_NAME (sym)),
		  gdb_stdout, 0);

      printf_filtered (";\n");
    }
}

/* This help function for symtab_symbol_info() prints information
   for non-debugging symbols to gdb_stdout.
 */
static void
print_msymbol_info (struct minimal_symbol *msymbol)
{
  char *tmp;

  if (gdbarch_addr_bit (current_gdbarch) <= 32)
    tmp = hex_string_custom (SYMBOL_VALUE_ADDRESS (msymbol)
			     & (CORE_ADDR) 0xffffffff,
			     8);
  else
    tmp = hex_string_custom (SYMBOL_VALUE_ADDRESS (msymbol),
			     16);
  printf_filtered ("%s  %s\n",
		   tmp, SYMBOL_PRINT_NAME (msymbol));
}

/* This is the guts of the commands "info functions", "info types", and
   "info variables". It calls search_symbols to find all matches and then
   print_[m]symbol_info to print out some useful information about the
   matches.
 */
static void
symtab_symbol_info (char *regexp, domain_enum kind, int from_tty)
{
  static char *classnames[]
  =
  {"variable", "function", "type", "method"};
  struct symbol_search *symbols;
  struct symbol_search *p;
  struct cleanup *old_chain;
  char *last_filename = NULL;
  int first = 1;

  /* must make sure that if we're interrupted, symbols gets freed */
  search_symbols (regexp, kind, 0, (char **) NULL, &symbols);
  old_chain = make_cleanup_free_search_symbols (symbols);

  printf_filtered (regexp
		   ? "All %ss matching regular expression \"%s\":\n"
		   : "All defined %ss:\n",
		   classnames[(int) (kind - VARIABLES_DOMAIN)], regexp);

  for (p = symbols; p != NULL; p = p->next)
    {
      QUIT;

      if (p->msymbol != NULL)
	{
	  if (first)
	    {
	      printf_filtered ("\nNon-debugging symbols:\n");
	      first = 0;
	    }
	  print_msymbol_info (p->msymbol);
	}
      else
	{
	  print_symbol_info (kind,
			     p->symtab,
			     p->symbol,
			     p->block,
			     last_filename);
	  last_filename = p->symtab->filename;
	}
    }

  do_cleanups (old_chain);
}

static void
variables_info (char *regexp, int from_tty)
{
  symtab_symbol_info (regexp, VARIABLES_DOMAIN, from_tty);
}

static void
functions_info (char *regexp, int from_tty)
{
  symtab_symbol_info (regexp, FUNCTIONS_DOMAIN, from_tty);
}


static void
types_info (char *regexp, int from_tty)
{
  symtab_symbol_info (regexp, TYPES_DOMAIN, from_tty);
}

/* Breakpoint all functions matching regular expression. */

void
rbreak_command_wrapper (char *regexp, int from_tty)
{
  rbreak_command (regexp, from_tty);
}

static void
rbreak_command (char *regexp, int from_tty)
{
  struct symbol_search *ss;
  struct symbol_search *p;
  struct cleanup *old_chain;

  search_symbols (regexp, FUNCTIONS_DOMAIN, 0, (char **) NULL, &ss);
  old_chain = make_cleanup_free_search_symbols (ss);

  for (p = ss; p != NULL; p = p->next)
    {
      if (p->msymbol == NULL)
	{
	  char *string = alloca (strlen (p->symtab->filename)
				 + strlen (SYMBOL_LINKAGE_NAME (p->symbol))
				 + 4);
	  strcpy (string, p->symtab->filename);
	  strcat (string, ":'");
	  strcat (string, SYMBOL_LINKAGE_NAME (p->symbol));
	  strcat (string, "'");
	  break_command (string, from_tty);
	  print_symbol_info (FUNCTIONS_DOMAIN,
			     p->symtab,
			     p->symbol,
			     p->block,
			     p->symtab->filename);
	}
      else
	{
	  char *string = alloca (strlen (SYMBOL_LINKAGE_NAME (p->msymbol))
				 + 3);
	  strcpy (string, "'");
	  strcat (string, SYMBOL_LINKAGE_NAME (p->msymbol));
	  strcat (string, "'");

	  break_command (string, from_tty);
	  printf_filtered ("<function, no debug info> %s;\n",
			   SYMBOL_PRINT_NAME (p->msymbol));
	}
    }

  do_cleanups (old_chain);
}


/* Helper routine for make_symbol_completion_list.  */

static int return_val_size;
static int return_val_index;
static char **return_val;

#define COMPLETION_LIST_ADD_SYMBOL(symbol, sym_text, len, text, word) \
      completion_list_add_name \
	(SYMBOL_NATURAL_NAME (symbol), (sym_text), (len), (text), (word))

/*  Test to see if the symbol specified by SYMNAME (which is already
   demangled for C++ symbols) matches SYM_TEXT in the first SYM_TEXT_LEN
   characters.  If so, add it to the current completion list. */

static void
completion_list_add_name (char *symname, char *sym_text, int sym_text_len,
			  char *text, char *word)
{
  int newsize;
  int i;

  /* clip symbols that cannot match */

  if (strncmp (symname, sym_text, sym_text_len) != 0)
    {
      return;
    }

  /* We have a match for a completion, so add SYMNAME to the current list
     of matches. Note that the name is moved to freshly malloc'd space. */

  {
    char *new;
    if (word == sym_text)
      {
	new = xmalloc (strlen (symname) + 5);
	strcpy (new, symname);
      }
    else if (word > sym_text)
      {
	/* Return some portion of symname.  */
	new = xmalloc (strlen (symname) + 5);
	strcpy (new, symname + (word - sym_text));
      }
    else
      {
	/* Return some of SYM_TEXT plus symname.  */
	new = xmalloc (strlen (symname) + (sym_text - word) + 5);
	strncpy (new, word, sym_text - word);
	new[sym_text - word] = '\0';
	strcat (new, symname);
      }

    if (return_val_index + 3 > return_val_size)
      {
	newsize = (return_val_size *= 2) * sizeof (char *);
	return_val = (char **) xrealloc ((char *) return_val, newsize);
      }
    return_val[return_val_index++] = new;
    return_val[return_val_index] = NULL;
  }
}

/* ObjC: In case we are completing on a selector, look as the msymbol
   again and feed all the selectors into the mill.  */

static void
completion_list_objc_symbol (struct minimal_symbol *msymbol, char *sym_text,
			     int sym_text_len, char *text, char *word)
{
  static char *tmp = NULL;
  static unsigned int tmplen = 0;
    
  char *method, *category, *selector;
  char *tmp2 = NULL;
    
  method = SYMBOL_NATURAL_NAME (msymbol);

  /* Is it a method?  */
  if ((method[0] != '-') && (method[0] != '+'))
    return;

  if (sym_text[0] == '[')
    /* Complete on shortened method method.  */
    completion_list_add_name (method + 1, sym_text, sym_text_len, text, word);
    
  while ((strlen (method) + 1) >= tmplen)
    {
      if (tmplen == 0)
	tmplen = 1024;
      else
	tmplen *= 2;
      tmp = xrealloc (tmp, tmplen);
    }
  selector = strchr (method, ' ');
  if (selector != NULL)
    selector++;
    
  category = strchr (method, '(');
    
  if ((category != NULL) && (selector != NULL))
    {
      memcpy (tmp, method, (category - method));
      tmp[category - method] = ' ';
      memcpy (tmp + (category - method) + 1, selector, strlen (selector) + 1);
      completion_list_add_name (tmp, sym_text, sym_text_len, text, word);
      if (sym_text[0] == '[')
	completion_list_add_name (tmp + 1, sym_text, sym_text_len, text, word);
    }
    
  if (selector != NULL)
    {
      /* Complete on selector only.  */
      strcpy (tmp, selector);
      tmp2 = strchr (tmp, ']');
      if (tmp2 != NULL)
	*tmp2 = '\0';
	
      completion_list_add_name (tmp, sym_text, sym_text_len, text, word);
    }
}

/* Break the non-quoted text based on the characters which are in
   symbols. FIXME: This should probably be language-specific. */

static char *
language_search_unquoted_string (char *text, char *p)
{
  for (; p > text; --p)
    {
      if (isalnum (p[-1]) || p[-1] == '_' || p[-1] == '\0')
	continue;
      else
	{
	  if ((current_language->la_language == language_objc))
	    {
	      if (p[-1] == ':')     /* might be part of a method name */
		continue;
	      else if (p[-1] == '[' && (p[-2] == '-' || p[-2] == '+'))
		p -= 2;             /* beginning of a method name */
	      else if (p[-1] == ' ' || p[-1] == '(' || p[-1] == ')')
		{                   /* might be part of a method name */
		  char *t = p;

		  /* Seeing a ' ' or a '(' is not conclusive evidence
		     that we are in the middle of a method name.  However,
		     finding "-[" or "+[" should be pretty un-ambiguous.
		     Unfortunately we have to find it now to decide.  */

		  while (t > text)
		    if (isalnum (t[-1]) || t[-1] == '_' ||
			t[-1] == ' '    || t[-1] == ':' ||
			t[-1] == '('    || t[-1] == ')')
		      --t;
		    else
		      break;

		  if (t[-1] == '[' && (t[-2] == '-' || t[-2] == '+'))
		    p = t - 2;      /* method name detected */
		  /* else we leave with p unchanged */
		}
	    }
	  break;
	}
    }
  return p;
}

char **
default_make_symbol_completion_list (char *text, char *word)
{
  /* Problem: All of the symbols have to be copied because readline
     frees them.  I'm not going to worry about this; hopefully there
     won't be that many.  */

  struct symbol *sym;
  struct symtab *s;
  struct partial_symtab *ps;
  struct minimal_symbol *msymbol;
  struct objfile *objfile;
  struct block *b, *surrounding_static_block = 0;
  struct dict_iterator iter;
  int j;
  struct partial_symbol **psym;
  /* The symbol we are completing on.  Points in same buffer as text.  */
  char *sym_text;
  /* Length of sym_text.  */
  int sym_text_len;

  /* Now look for the symbol we are supposed to complete on.  */
  {
    char *p;
    char quote_found;
    char *quote_pos = NULL;

    /* First see if this is a quoted string.  */
    quote_found = '\0';
    for (p = text; *p != '\0'; ++p)
      {
	if (quote_found != '\0')
	  {
	    if (*p == quote_found)
	      /* Found close quote.  */
	      quote_found = '\0';
	    else if (*p == '\\' && p[1] == quote_found)
	      /* A backslash followed by the quote character
	         doesn't end the string.  */
	      ++p;
	  }
	else if (*p == '\'' || *p == '"')
	  {
	    quote_found = *p;
	    quote_pos = p;
	  }
      }
    if (quote_found == '\'')
      /* A string within single quotes can be a symbol, so complete on it.  */
      sym_text = quote_pos + 1;
    else if (quote_found == '"')
      /* A double-quoted string is never a symbol, nor does it make sense
         to complete it any other way.  */
      {
	return_val = (char **) xmalloc (sizeof (char *));
	return_val[0] = NULL;
	return return_val;
      }
    else
      {
	/* It is not a quoted string.  Break it based on the characters
	   which are in symbols.  */
	while (p > text)
	  {
	    if (isalnum (p[-1]) || p[-1] == '_' || p[-1] == '\0')
	      --p;
	    else
	      break;
	  }
	sym_text = p;
      }
  }

  sym_text_len = strlen (sym_text);

  return_val_size = 100;
  return_val_index = 0;
  return_val = (char **) xmalloc ((return_val_size + 1) * sizeof (char *));
  return_val[0] = NULL;

  /* Look through the partial symtabs for all symbols which begin
     by matching SYM_TEXT.  Add each one that you find to the list.  */

  ALL_PSYMTABS (objfile, ps)
  {
    /* If the psymtab's been read in we'll get it when we search
       through the blockvector.  */
    if (ps->readin)
      continue;

    for (psym = objfile->global_psymbols.list + ps->globals_offset;
	 psym < (objfile->global_psymbols.list + ps->globals_offset
		 + ps->n_global_syms);
	 psym++)
      {
	/* If interrupted, then quit. */
	QUIT;
	COMPLETION_LIST_ADD_SYMBOL (*psym, sym_text, sym_text_len, text, word);
      }

    for (psym = objfile->static_psymbols.list + ps->statics_offset;
	 psym < (objfile->static_psymbols.list + ps->statics_offset
		 + ps->n_static_syms);
	 psym++)
      {
	QUIT;
	COMPLETION_LIST_ADD_SYMBOL (*psym, sym_text, sym_text_len, text, word);
      }
  }

  /* At this point scan through the misc symbol vectors and add each
     symbol you find to the list.  Eventually we want to ignore
     anything that isn't a text symbol (everything else will be
     handled by the psymtab code above).  */

  ALL_MSYMBOLS (objfile, msymbol)
  {
    QUIT;
    COMPLETION_LIST_ADD_SYMBOL (msymbol, sym_text, sym_text_len, text, word);
    
    completion_list_objc_symbol (msymbol, sym_text, sym_text_len, text, word);
  }

  /* Search upwards from currently selected frame (so that we can
     complete on local vars.  */

  for (b = get_selected_block (0); b != NULL; b = BLOCK_SUPERBLOCK (b))
    {
      if (!BLOCK_SUPERBLOCK (b))
	{
	  surrounding_static_block = b;		/* For elmin of dups */
	}

      /* Also catch fields of types defined in this places which match our
         text string.  Only complete on types visible from current context. */

      ALL_BLOCK_SYMBOLS (b, iter, sym)
	{
	  QUIT;
	  COMPLETION_LIST_ADD_SYMBOL (sym, sym_text, sym_text_len, text, word);
	  if (SYMBOL_CLASS (sym) == LOC_TYPEDEF)
	    {
	      struct type *t = SYMBOL_TYPE (sym);
	      enum type_code c = TYPE_CODE (t);

	      if (c == TYPE_CODE_UNION || c == TYPE_CODE_STRUCT)
		{
		  for (j = TYPE_N_BASECLASSES (t); j < TYPE_NFIELDS (t); j++)
		    {
		      if (TYPE_FIELD_NAME (t, j))
			{
			  completion_list_add_name (TYPE_FIELD_NAME (t, j),
					sym_text, sym_text_len, text, word);
			}
		    }
		}
	    }
	}
    }

  /* Go through the symtabs and check the externs and statics for
     symbols which match.  */

  ALL_PRIMARY_SYMTABS (objfile, s)
  {
    QUIT;
    b = BLOCKVECTOR_BLOCK (BLOCKVECTOR (s), GLOBAL_BLOCK);
    ALL_BLOCK_SYMBOLS (b, iter, sym)
      {
	COMPLETION_LIST_ADD_SYMBOL (sym, sym_text, sym_text_len, text, word);
      }
  }

  ALL_PRIMARY_SYMTABS (objfile, s)
  {
    QUIT;
    b = BLOCKVECTOR_BLOCK (BLOCKVECTOR (s), STATIC_BLOCK);
    /* Don't do this block twice.  */
    if (b == surrounding_static_block)
      continue;
    ALL_BLOCK_SYMBOLS (b, iter, sym)
      {
	COMPLETION_LIST_ADD_SYMBOL (sym, sym_text, sym_text_len, text, word);
      }
  }

  return (return_val);
}

/* Return a NULL terminated array of all symbols (regardless of class)
   which begin by matching TEXT.  If the answer is no symbols, then
   the return value is an array which contains only a NULL pointer.  */

char **
make_symbol_completion_list (char *text, char *word)
{
  return current_language->la_make_symbol_completion_list (text, word);
}

/* Like make_symbol_completion_list, but returns a list of symbols
   defined in a source file FILE.  */

char **
make_file_symbol_completion_list (char *text, char *word, char *srcfile)
{
  struct symbol *sym;
  struct symtab *s;
  struct block *b;
  struct dict_iterator iter;
  /* The symbol we are completing on.  Points in same buffer as text.  */
  char *sym_text;
  /* Length of sym_text.  */
  int sym_text_len;

  /* Now look for the symbol we are supposed to complete on.
     FIXME: This should be language-specific.  */
  {
    char *p;
    char quote_found;
    char *quote_pos = NULL;

    /* First see if this is a quoted string.  */
    quote_found = '\0';
    for (p = text; *p != '\0'; ++p)
      {
	if (quote_found != '\0')
	  {
	    if (*p == quote_found)
	      /* Found close quote.  */
	      quote_found = '\0';
	    else if (*p == '\\' && p[1] == quote_found)
	      /* A backslash followed by the quote character
	         doesn't end the string.  */
	      ++p;
	  }
	else if (*p == '\'' || *p == '"')
	  {
	    quote_found = *p;
	    quote_pos = p;
	  }
      }
    if (quote_found == '\'')
      /* A string within single quotes can be a symbol, so complete on it.  */
      sym_text = quote_pos + 1;
    else if (quote_found == '"')
      /* A double-quoted string is never a symbol, nor does it make sense
         to complete it any other way.  */
      {
	return_val = (char **) xmalloc (sizeof (char *));
	return_val[0] = NULL;
	return return_val;
      }
    else
      {
	/* Not a quoted string.  */
	sym_text = language_search_unquoted_string (text, p);
      }
  }

  sym_text_len = strlen (sym_text);

  return_val_size = 10;
  return_val_index = 0;
  return_val = (char **) xmalloc ((return_val_size + 1) * sizeof (char *));
  return_val[0] = NULL;

  /* Find the symtab for SRCFILE (this loads it if it was not yet read
     in).  */
  s = lookup_symtab (srcfile);
  if (s == NULL)
    {
      /* Maybe they typed the file with leading directories, while the
	 symbol tables record only its basename.  */
      const char *tail = lbasename (srcfile);

      if (tail > srcfile)
	s = lookup_symtab (tail);
    }

  /* If we have no symtab for that file, return an empty list.  */
  if (s == NULL)
    return (return_val);

  /* Go through this symtab and check the externs and statics for
     symbols which match.  */

  b = BLOCKVECTOR_BLOCK (BLOCKVECTOR (s), GLOBAL_BLOCK);
  ALL_BLOCK_SYMBOLS (b, iter, sym)
    {
      COMPLETION_LIST_ADD_SYMBOL (sym, sym_text, sym_text_len, text, word);
    }

  b = BLOCKVECTOR_BLOCK (BLOCKVECTOR (s), STATIC_BLOCK);
  ALL_BLOCK_SYMBOLS (b, iter, sym)
    {
      COMPLETION_LIST_ADD_SYMBOL (sym, sym_text, sym_text_len, text, word);
    }

  return (return_val);
}

/* A helper function for make_source_files_completion_list.  It adds
   another file name to a list of possible completions, growing the
   list as necessary.  */

static void
add_filename_to_list (const char *fname, char *text, char *word,
		      char ***list, int *list_used, int *list_alloced)
{
  char *new;
  size_t fnlen = strlen (fname);

  if (*list_used + 1 >= *list_alloced)
    {
      *list_alloced *= 2;
      *list = (char **) xrealloc ((char *) *list,
				  *list_alloced * sizeof (char *));
    }

  if (word == text)
    {
      /* Return exactly fname.  */
      new = xmalloc (fnlen + 5);
      strcpy (new, fname);
    }
  else if (word > text)
    {
      /* Return some portion of fname.  */
      new = xmalloc (fnlen + 5);
      strcpy (new, fname + (word - text));
    }
  else
    {
      /* Return some of TEXT plus fname.  */
      new = xmalloc (fnlen + (text - word) + 5);
      strncpy (new, word, text - word);
      new[text - word] = '\0';
      strcat (new, fname);
    }
  (*list)[*list_used] = new;
  (*list)[++*list_used] = NULL;
}

static int
not_interesting_fname (const char *fname)
{
  static const char *illegal_aliens[] = {
    "_globals_",	/* inserted by coff_symtab_read */
    NULL
  };
  int i;

  for (i = 0; illegal_aliens[i]; i++)
    {
      if (strcmp (fname, illegal_aliens[i]) == 0)
	return 1;
    }
  return 0;
}

/* Return a NULL terminated array of all source files whose names
   begin with matching TEXT.  The file names are looked up in the
   symbol tables of this program.  If the answer is no matchess, then
   the return value is an array which contains only a NULL pointer.  */

char **
make_source_files_completion_list (char *text, char *word)
{
  struct symtab *s;
  struct partial_symtab *ps;
  struct objfile *objfile;
  int first = 1;
  int list_alloced = 1;
  int list_used = 0;
  size_t text_len = strlen (text);
  char **list = (char **) xmalloc (list_alloced * sizeof (char *));
  const char *base_name;

  list[0] = NULL;

  if (!have_full_symbols () && !have_partial_symbols ())
    return list;

  ALL_SYMTABS (objfile, s)
    {
      if (not_interesting_fname (s->filename))
	continue;
      if (!filename_seen (s->filename, 1, &first)
#if HAVE_DOS_BASED_FILE_SYSTEM
	  && strncasecmp (s->filename, text, text_len) == 0
#else
	  && strncmp (s->filename, text, text_len) == 0
#endif
	  )
	{
	  /* This file matches for a completion; add it to the current
	     list of matches.  */
	  add_filename_to_list (s->filename, text, word,
				&list, &list_used, &list_alloced);
	}
      else
	{
	  /* NOTE: We allow the user to type a base name when the
	     debug info records leading directories, but not the other
	     way around.  This is what subroutines of breakpoint
	     command do when they parse file names.  */
	  base_name = lbasename (s->filename);
	  if (base_name != s->filename
	      && !filename_seen (base_name, 1, &first)
#if HAVE_DOS_BASED_FILE_SYSTEM
	      && strncasecmp (base_name, text, text_len) == 0
#else
	      && strncmp (base_name, text, text_len) == 0
#endif
	      )
	    add_filename_to_list (base_name, text, word,
				  &list, &list_used, &list_alloced);
	}
    }

  ALL_PSYMTABS (objfile, ps)
    {
      if (not_interesting_fname (ps->filename))
	continue;
      if (!ps->readin)
	{
	  if (!filename_seen (ps->filename, 1, &first)
#if HAVE_DOS_BASED_FILE_SYSTEM
	      && strncasecmp (ps->filename, text, text_len) == 0
#else
	      && strncmp (ps->filename, text, text_len) == 0
#endif
	      )
	    {
	      /* This file matches for a completion; add it to the
		 current list of matches.  */
	      add_filename_to_list (ps->filename, text, word,
				    &list, &list_used, &list_alloced);

	    }
	  else
	    {
	      base_name = lbasename (ps->filename);
	      if (base_name != ps->filename
		  && !filename_seen (base_name, 1, &first)
#if HAVE_DOS_BASED_FILE_SYSTEM
		  && strncasecmp (base_name, text, text_len) == 0
#else
		  && strncmp (base_name, text, text_len) == 0
#endif
		  )
		add_filename_to_list (base_name, text, word,
				      &list, &list_used, &list_alloced);
	    }
	}
    }

  return list;
}

/* Determine if PC is in the prologue of a function.  The prologue is the area
   between the first instruction of a function, and the first executable line.
   Returns 1 if PC *might* be in prologue, 0 if definately *not* in prologue.

   If non-zero, func_start is where we think the prologue starts, possibly
   by previous examination of symbol table information.
 */

int
in_prologue (CORE_ADDR pc, CORE_ADDR func_start)
{
  struct symtab_and_line sal;
  CORE_ADDR func_addr, func_end;

  /* We have several sources of information we can consult to figure
     this out.
     - Compilers usually emit line number info that marks the prologue
       as its own "source line".  So the ending address of that "line"
       is the end of the prologue.  If available, this is the most
       reliable method.
     - The minimal symbols and partial symbols, which can usually tell
       us the starting and ending addresses of a function.
     - If we know the function's start address, we can call the
       architecture-defined gdbarch_skip_prologue function to analyze the
       instruction stream and guess where the prologue ends.
     - Our `func_start' argument; if non-zero, this is the caller's
       best guess as to the function's entry point.  At the time of
       this writing, handle_inferior_event doesn't get this right, so
       it should be our last resort.  */

  /* Consult the partial symbol table, to find which function
     the PC is in.  */
  if (! find_pc_partial_function (pc, NULL, &func_addr, &func_end))
    {
      CORE_ADDR prologue_end;

      /* We don't even have minsym information, so fall back to using
         func_start, if given.  */
      if (! func_start)
	return 1;		/* We *might* be in a prologue.  */

      prologue_end = gdbarch_skip_prologue (current_gdbarch, func_start);

      return func_start <= pc && pc < prologue_end;
    }

  /* If we have line number information for the function, that's
     usually pretty reliable.  */
  sal = find_pc_line (func_addr, 0);

  /* Now sal describes the source line at the function's entry point,
     which (by convention) is the prologue.  The end of that "line",
     sal.end, is the end of the prologue.

     Note that, for functions whose source code is all on a single
     line, the line number information doesn't always end up this way.
     So we must verify that our purported end-of-prologue address is
     *within* the function, not at its start or end.  */
  if (sal.line == 0
      || sal.end <= func_addr
      || func_end <= sal.end)
    {
      /* We don't have any good line number info, so use the minsym
	 information, together with the architecture-specific prologue
	 scanning code.  */
      CORE_ADDR prologue_end = gdbarch_skip_prologue
			         (current_gdbarch, func_addr);

      return func_addr <= pc && pc < prologue_end;
    }

  /* We have line number info, and it looks good.  */
  return func_addr <= pc && pc < sal.end;
}

/* Given PC at the function's start address, attempt to find the
   prologue end using SAL information.  Return zero if the skip fails.

   A non-optimized prologue traditionally has one SAL for the function
   and a second for the function body.  A single line function has
   them both pointing at the same line.

   An optimized prologue is similar but the prologue may contain
   instructions (SALs) from the instruction body.  Need to skip those
   while not getting into the function body.

   The functions end point and an increasing SAL line are used as
   indicators of the prologue's endpoint.

   This code is based on the function refine_prologue_limit (versions
   found in both ia64 and ppc).  */

CORE_ADDR
skip_prologue_using_sal (CORE_ADDR func_addr)
{
  struct symtab_and_line prologue_sal;
  CORE_ADDR start_pc;
  CORE_ADDR end_pc;

  /* Get an initial range for the function.  */
  find_pc_partial_function (func_addr, NULL, &start_pc, &end_pc);
  start_pc += gdbarch_deprecated_function_start_offset (current_gdbarch);

  prologue_sal = find_pc_line (start_pc, 0);
  if (prologue_sal.line != 0)
    {
      /* If there is only one sal that covers the entire function,
	 then it is probably a single line function, like
	 "foo(){}". */
      if (prologue_sal.end >= end_pc)
	return 0;
      while (prologue_sal.end < end_pc)
	{
	  struct symtab_and_line sal;

	  sal = find_pc_line (prologue_sal.end, 0);
	  if (sal.line == 0)
	    break;
	  /* Assume that a consecutive SAL for the same (or larger)
	     line mark the prologue -> body transition.  */
	  if (sal.line >= prologue_sal.line)
	    break;
	  /* The case in which compiler's optimizer/scheduler has
	     moved instructions into the prologue.  We look ahead in
	     the function looking for address ranges whose
	     corresponding line number is less the first one that we
	     found for the function.  This is more conservative then
	     refine_prologue_limit which scans a large number of SALs
	     looking for any in the prologue */
	  prologue_sal = sal;
	}
    }
  return prologue_sal.end;
}

struct symtabs_and_lines
decode_line_spec (char *string, int funfirstline)
{
  struct symtabs_and_lines sals;
  struct symtab_and_line cursal;
  
  if (string == 0)
    error (_("Empty line specification."));
    
  /* We use whatever is set as the current source line. We do not try
     and get a default  or it will recursively call us! */  
  cursal = get_current_source_symtab_and_line ();
  
  sals = decode_line_1 (&string, funfirstline,
			cursal.symtab, cursal.line,
			(char ***) NULL, NULL);

  if (*string)
    error (_("Junk at end of line specification: %s"), string);
  return sals;
}

/* Track MAIN */
static char *name_of_main;

void
set_main_name (const char *name)
{
  if (name_of_main != NULL)
    {
      xfree (name_of_main);
      name_of_main = NULL;
    }
  if (name != NULL)
    {
      name_of_main = xstrdup (name);
    }
}

/* Deduce the name of the main procedure, and set NAME_OF_MAIN
   accordingly.  */

static void
find_main_name (void)
{
  const char *new_main_name;

  /* Try to see if the main procedure is in Ada.  */
  /* FIXME: brobecker/2005-03-07: Another way of doing this would
     be to add a new method in the language vector, and call this
     method for each language until one of them returns a non-empty
     name.  This would allow us to remove this hard-coded call to
     an Ada function.  It is not clear that this is a better approach
     at this point, because all methods need to be written in a way
     such that false positives never be returned. For instance, it is
     important that a method does not return a wrong name for the main
     procedure if the main procedure is actually written in a different
     language.  It is easy to guaranty this with Ada, since we use a
     special symbol generated only when the main in Ada to find the name
     of the main procedure. It is difficult however to see how this can
     be guarantied for languages such as C, for instance.  This suggests
     that order of call for these methods becomes important, which means
     a more complicated approach.  */
  new_main_name = ada_main_name ();
  if (new_main_name != NULL)
    { 
      set_main_name (new_main_name);
      return;
    }

  new_main_name = pascal_main_name ();
  if (new_main_name != NULL)
    { 
      set_main_name (new_main_name);
      return;
    }

  /* The languages above didn't identify the name of the main procedure.
     Fallback to "main".  */
  set_main_name ("main");
}

char *
main_name (void)
{
  if (name_of_main == NULL)
    find_main_name ();

  return name_of_main;
}

/* Handle ``executable_changed'' events for the symtab module.  */

static void
symtab_observer_executable_changed (void)
{
  /* NAME_OF_MAIN may no longer be the same, so reset it for now.  */
  set_main_name (NULL);
}

/* Helper to expand_line_sal below.  Appends new sal to SAL,
   initializing it from SYMTAB, LINENO and PC.  */
static void
append_expanded_sal (struct symtabs_and_lines *sal,
		     struct symtab *symtab,
		     int lineno, CORE_ADDR pc)
{
  CORE_ADDR func_addr, func_end;
  
  sal->sals = xrealloc (sal->sals, 
			sizeof (sal->sals[0]) 
			* (sal->nelts + 1));
  init_sal (sal->sals + sal->nelts);
  sal->sals[sal->nelts].symtab = symtab;
  sal->sals[sal->nelts].section = NULL;
  sal->sals[sal->nelts].end = 0;
  sal->sals[sal->nelts].line = lineno;  
  sal->sals[sal->nelts].pc = pc;
  ++sal->nelts;      
}

/* Compute a set of all sals in
   the entire program that correspond to same file
   and line as SAL and return those.  If there
   are several sals that belong to the same block,
   only one sal for the block is included in results.  */
   
struct symtabs_and_lines
expand_line_sal (struct symtab_and_line sal)
{
  struct symtabs_and_lines ret, this_line;
  int i, j;
  struct objfile *objfile;
  struct partial_symtab *psymtab;
  struct symtab *symtab;
  int lineno;
  int deleted = 0;
  struct block **blocks = NULL;
  int *filter;

  ret.nelts = 0;
  ret.sals = NULL;

  if (sal.symtab == NULL || sal.line == 0 || sal.pc != 0)
    {
      ret.sals = xmalloc (sizeof (struct symtab_and_line));
      ret.sals[0] = sal;
      ret.nelts = 1;
      return ret;
    }
  else
    {
      struct linetable_entry *best_item = 0;
      struct symtab *best_symtab = 0;
      int exact = 0;

      lineno = sal.line;

      /* We meed to find all symtabs for a file which name
	 is described by sal. We cannot just directly 
	 iterate over symtabs, since a symtab might not be
	 yet created. We also cannot iterate over psymtabs,
	 calling PSYMTAB_TO_SYMTAB and working on that symtab,
	 since PSYMTAB_TO_SYMTAB will return NULL for psymtab
	 corresponding to an included file. Therefore, we do
	 first pass over psymtabs, reading in those with
	 the right name.  Then, we iterate over symtabs, knowing
	 that all symtabs we're interested in are loaded.  */

      ALL_PSYMTABS (objfile, psymtab)
	{
	  if (strcmp (sal.symtab->filename,
		      psymtab->filename) == 0)
	    PSYMTAB_TO_SYMTAB (psymtab);
	}

	 
      /* For each symtab, we add all pcs to ret.sals. I'm actually
	 not sure what to do if we have exact match in one symtab,
	 and non-exact match on another symtab.
      */
      ALL_SYMTABS (objfile, symtab)
	{
	  if (strcmp (sal.symtab->filename,
		      symtab->filename) == 0)
	    {
	      struct linetable *l;
	      int len;
	      l = LINETABLE (symtab);
	      if (!l)
		continue;
	      len = l->nitems;

	      for (j = 0; j < len; j++)
		{
		  struct linetable_entry *item = &(l->item[j]);

		  if (item->line == lineno)
		    {
		      exact = 1;
		      append_expanded_sal (&ret, symtab, lineno, item->pc);
		    }      
		  else if (!exact && item->line > lineno
			   && (best_item == NULL || item->line < best_item->line))
		  
		    {
		      best_item = item;
		      best_symtab = symtab;
		    }
		}
	    }
	}
      if (!exact && best_item)
	append_expanded_sal (&ret, best_symtab, lineno, best_item->pc);
    }

  /* For optimized code, compiler can scatter one source line accross
     disjoint ranges of PC values, even when no duplicate functions
     or inline functions are involved.  For example, 'for (;;)' inside
     non-template non-inline non-ctor-or-dtor function can result
     in two PC ranges.  In this case, we don't want to set breakpoint
     on first PC of each range.  To filter such cases, we use containing
     blocks -- for each PC found above we see if there are other PCs
     that are in the same block.  If yes, the other PCs are filtered out.  */  

  filter = alloca (ret.nelts * sizeof (int));
  blocks = alloca (ret.nelts * sizeof (struct block *));
  for (i = 0; i < ret.nelts; ++i)
    {
      filter[i] = 1;
      blocks[i] = block_for_pc (ret.sals[i].pc);
    }

  for (i = 0; i < ret.nelts; ++i)
    if (blocks[i] != NULL)
      for (j = i+1; j < ret.nelts; ++j)
	if (blocks[j] == blocks[i])
	  {
	    filter[j] = 0;
	    ++deleted;
	    break;
	  }
  
  {
    struct symtab_and_line *final = 
      xmalloc (sizeof (struct symtab_and_line) * (ret.nelts-deleted));
    
    for (i = 0, j = 0; i < ret.nelts; ++i)
      if (filter[i])
	final[j++] = ret.sals[i];
    
    ret.nelts -= deleted;
    xfree (ret.sals);
    ret.sals = final;
  }

  return ret;
}


void
_initialize_symtab (void)
{
  add_info ("variables", variables_info, _("\
All global and static variable names, or those matching REGEXP."));
  if (dbx_commands)
    add_com ("whereis", class_info, variables_info, _("\
All global and static variable names, or those matching REGEXP."));

  add_info ("functions", functions_info,
	    _("All function names, or those matching REGEXP."));

  
  /* FIXME:  This command has at least the following problems:
     1.  It prints builtin types (in a very strange and confusing fashion).
     2.  It doesn't print right, e.g. with
     typedef struct foo *FOO
     type_print prints "FOO" when we want to make it (in this situation)
     print "struct foo *".
     I also think "ptype" or "whatis" is more likely to be useful (but if
     there is much disagreement "info types" can be fixed).  */
  add_info ("types", types_info,
	    _("All type names, or those matching REGEXP."));

  add_info ("sources", sources_info,
	    _("Source files in the program."));

  add_com ("rbreak", class_breakpoint, rbreak_command,
	   _("Set a breakpoint for all functions matching REGEXP."));

  if (xdb_commands)
    {
      add_com ("lf", class_info, sources_info,
	       _("Source files in the program"));
      add_com ("lg", class_info, variables_info, _("\
All global and static variable names, or those matching REGEXP."));
    }

  add_setshow_enum_cmd ("multiple-symbols", no_class,
                        multiple_symbols_modes, &multiple_symbols_mode,
                        _("\
Set the debugger behavior when more than one symbol are possible matches\n\
in an expression."), _("\
Show how the debugger handles ambiguities in expressions."), _("\
Valid values are \"ask\", \"all\", \"cancel\", and the default is \"all\"."),
                        NULL, NULL, &setlist, &showlist);

  /* Initialize the one built-in type that isn't language dependent... */
  builtin_type_error = init_type (TYPE_CODE_ERROR, 0, 0,
				  "<unknown type>", (struct objfile *) NULL);

  observer_attach_executable_changed (symtab_observer_executable_changed);
}
