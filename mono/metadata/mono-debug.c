/*
 * mono-debug.c: 
 *
 * Author:
 *	Mono Project (http://www.mono-project.com)
 *
 * Copyright 2001-2003 Ximian, Inc (http://www.ximian.com)
 * Copyright 2004-2009 Novell, Inc (http://www.novell.com)
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 */

#include <config.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/mono-debug-debugger.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/gc-internal.h>
#include <mono/metadata/mempool.h>
#include <string.h>

#define ALIGN_TO(val,align) ((((guint64)val) + ((align) - 1)) & ~((align) - 1))

#if NO_UNALIGNED_ACCESS
#define WRITE_UNALIGNED(type, addr, val) \
	memcpy(addr, &val, sizeof(type))
#define READ_UNALIGNED(type, addr, val) \
	memcpy(&val, addr, sizeof(type))
#else
#define WRITE_UNALIGNED(type, addr, val) \
	(*(type *)(addr) = (val))
#define READ_UNALIGNED(type, addr, val) \
	val = (*(type *)(addr))
#endif

/* This contains per-domain info */
struct _MonoDebugDataTable {
	MonoMemPool *mp;
	GHashTable *method_address_hash;
};

/* This contains JIT debugging information about a method in serialized format */
struct _MonoDebugMethodAddress {
	const guint8 *code_start;
	guint32 code_size;
	guint8 data [MONO_ZERO_LEN_ARRAY];
};

static MonoDebugFormat mono_debug_format = MONO_DEBUG_FORMAT_NONE;

static gboolean mono_debug_initialized = FALSE;
/* Maps MonoImage -> MonoMonoDebugHandle */
static GHashTable *mono_debug_handles;
/* Maps MonoDomain -> MonoDataTable */
static GHashTable *data_table_hash;

static mono_mutex_t debugger_lock_mutex;

static int initialized = 0;
static gboolean is_attached = FALSE;

static MonoDebugHandle     *mono_debug_open_image      (MonoImage *image, const guint8 *raw_contents, int size);

static MonoDebugHandle     *mono_debug_get_image      (MonoImage *image);
static void                 mono_debug_add_assembly    (MonoAssembly *assembly,
							gpointer user_data);

static MonoDebugHandle     *open_symfile_from_bundle   (MonoImage *image);

static MonoDebugDataTable *
create_data_table (MonoDomain *domain)
{
	MonoDebugDataTable *table;

	table = g_new0 (MonoDebugDataTable, 1);

	table->mp = mono_mempool_new ();
	table->method_address_hash = g_hash_table_new (NULL, NULL);

	if (domain)
		g_hash_table_insert (data_table_hash, domain, table);

	return table;
}

static void
free_data_table (MonoDebugDataTable *table)
{
	mono_mempool_destroy (table->mp);
	g_hash_table_destroy (table->method_address_hash);

	g_free (table);
}

static MonoDebugDataTable *
lookup_data_table (MonoDomain *domain)
{
	MonoDebugDataTable *table;

	table = g_hash_table_lookup (data_table_hash, domain);
	if (!table) {
		g_error ("lookup_data_table () failed for %p\n", domain);
		g_assert (table);
	}
	return table;
}

static void
free_debug_handle (MonoDebugHandle *handle)
{
	if (handle->symfile)
		mono_debug_close_mono_symbol_file (handle->symfile);
	/* decrease the refcount added with mono_image_addref () */
	mono_image_close (handle->image);
	g_free (handle);
}

/*
 * Initialize debugging support.
 *
 * This method must be called after loading corlib,
 * but before opening the application's main assembly because we need to set some
 * callbacks here.
 */
void
mono_debug_init (MonoDebugFormat format)
{
	g_assert (!mono_debug_initialized);
	if (format == MONO_DEBUG_FORMAT_DEBUGGER)
		g_error ("The mdb debugger is no longer supported.");

	mono_debug_initialized = TRUE;
	mono_debug_format = format;

	mono_debugger_initialize ();

	mono_debugger_lock ();

	mono_debug_handles = g_hash_table_new_full
		(NULL, NULL, NULL, (GDestroyNotify) free_debug_handle);

	data_table_hash = g_hash_table_new_full (
		NULL, NULL, NULL, (GDestroyNotify) free_data_table);

	mono_install_assembly_load_hook (mono_debug_add_assembly, NULL);

	mono_debugger_unlock ();
}

void
mono_debug_open_image_from_memory (MonoImage *image, const guint8 *raw_contents, int size)
{
	if (!mono_debug_initialized)
		return;

	mono_debug_open_image (image, raw_contents, size);
}

void
mono_debug_cleanup (void)
{
	if (mono_debug_handles)
		g_hash_table_destroy (mono_debug_handles);
	mono_debug_handles = NULL;

	if (data_table_hash) {
		g_hash_table_destroy (data_table_hash);
		data_table_hash = NULL;
	}
}

void
mono_debug_domain_create (MonoDomain *domain)
{
	MonoDebugDataTable *table;

	if (!mono_debug_initialized)
		return;

	mono_debugger_lock ();

	table = create_data_table (domain);

	mono_debugger_unlock ();
}

void
mono_debug_domain_unload (MonoDomain *domain)
{
	MonoDebugDataTable *table;

	if (!mono_debug_initialized)
		return;

	mono_debugger_lock ();

	table = g_hash_table_lookup (data_table_hash, domain);
	if (!table) {
		g_warning (G_STRLOC ": unloading unknown domain %p / %d",
			   domain, mono_domain_get_id (domain));
		mono_debugger_unlock ();
		return;
	}

	g_hash_table_remove (data_table_hash, domain);

	mono_debugger_unlock ();
}

/*
 * LOCKING: Assumes the debug lock is held.
 */
static MonoDebugHandle *
mono_debug_get_image (MonoImage *image)
{
	return g_hash_table_lookup (mono_debug_handles, image);
}

void
mono_debug_close_image (MonoImage *image)
{
	MonoDebugHandle *handle;

	if (!mono_debug_initialized)
		return;

	mono_debugger_lock ();

	handle = mono_debug_get_image (image);
	if (!handle) {
		mono_debugger_unlock ();
		return;
	}

	g_hash_table_remove (mono_debug_handles, image);

	mono_debugger_unlock ();
}

static MonoDebugHandle *
mono_debug_open_image (MonoImage *image, const guint8 *raw_contents, int size)
{
	MonoDebugHandle *handle;

	if (mono_image_is_dynamic (image))
		return NULL;

	mono_debugger_lock ();

	handle = mono_debug_get_image (image);
	if (handle != NULL) {
		mono_debugger_unlock ();
		return handle;
	}

	handle = g_new0 (MonoDebugHandle, 1);

	handle->image = image;
	mono_image_addref (image);

	handle->symfile = mono_debug_open_mono_symbols (
		handle, raw_contents, size, FALSE);

	g_hash_table_insert (mono_debug_handles, image, handle);

	mono_debugger_unlock ();

	return handle;
}

static void
mono_debug_add_assembly (MonoAssembly *assembly, gpointer user_data)
{
	MonoDebugHandle *handle;
	MonoImage *image;

	mono_debugger_lock ();
	image = mono_assembly_get_image (assembly);
	handle = open_symfile_from_bundle (image);
	if (!handle)
		mono_debug_open_image (image, NULL, 0);
	mono_debugger_unlock ();
}

struct LookupMethodData
{
	MonoDebugMethodInfo *minfo;
	MonoMethod *method;
};

static void
lookup_method_func (gpointer key, gpointer value, gpointer user_data)
{
	MonoDebugHandle *handle = (MonoDebugHandle *) value;
	struct LookupMethodData *data = (struct LookupMethodData *) user_data;

	if (data->minfo)
		return;

	if (handle->symfile)
		data->minfo = mono_debug_symfile_lookup_method (handle, data->method);
}

static MonoDebugMethodInfo *
mono_debug_lookup_method_internal (MonoMethod *method)
{
	struct LookupMethodData data;

	data.minfo = NULL;
	data.method = method;

	if (!mono_debug_handles)
		return NULL;

	g_hash_table_foreach (mono_debug_handles, lookup_method_func, &data);
	return data.minfo;
}

/**
 * mono_debug_lookup_method:
 *
 * Lookup symbol file information for the method @method.  The returned
 * `MonoDebugMethodInfo' is a private structure, but it can be passed to
 * mono_debug_symfile_lookup_location().
 */
MonoDebugMethodInfo *
mono_debug_lookup_method (MonoMethod *method)
{
	MonoDebugMethodInfo *minfo;

	mono_debugger_lock ();
	minfo = mono_debug_lookup_method_internal (method);
	mono_debugger_unlock ();
	return minfo;
}

typedef struct
{
	gboolean found;
	MonoImage *image;
} LookupImageData;

static void
lookup_image_func (gpointer key, gpointer value, gpointer user_data)
{
	MonoDebugHandle *handle = (MonoDebugHandle *) value;
	LookupImageData *data = (LookupImageData *) user_data;

	if (data->found)
		return;

	if (handle->image == data->image)
		data->found = TRUE;
}

gboolean
mono_debug_image_has_debug_info (MonoImage *image)
{
	LookupImageData data;

	if (!mono_debug_handles)
		return FALSE;

	memset (&data, 0, sizeof (data));
	data.image = image;

	mono_debugger_lock ();
	g_hash_table_foreach (mono_debug_handles, lookup_image_func, &data);
	mono_debugger_unlock ();
	return data.found;
}

static inline void
write_leb128 (guint32 value, guint8 *ptr, guint8 **rptr)
{
	do {
		guint8 byte = value & 0x7f;
		value >>= 7;
		if (value)
			byte |= 0x80;
		*ptr++ = byte;
	} while (value);

	*rptr = ptr;
}

static inline void
write_sleb128 (gint32 value, guint8 *ptr, guint8 **rptr)
{
	gboolean more = 1;

	while (more) {
		guint8 byte = value & 0x7f;
		value >>= 7;

		if (((value == 0) && ((byte & 0x40) == 0)) || ((value == -1) && (byte & 0x40)))
			more = 0;
		else
			byte |= 0x80;
		*ptr++ = byte;
	}

	*rptr = ptr;
}

static void
write_variable (MonoDebugVarInfo *var, guint8 *ptr, guint8 **rptr)
{
	write_leb128 (var->index, ptr, &ptr);
	write_sleb128 (var->offset, ptr, &ptr);
	write_leb128 (var->size, ptr, &ptr);
	write_leb128 (var->begin_scope, ptr, &ptr);
	write_leb128 (var->end_scope, ptr, &ptr);
	WRITE_UNALIGNED (gpointer, ptr, var->type);
	ptr += sizeof (gpointer);
	*rptr = ptr;
}

MonoDebugMethodAddress *
mono_debug_add_method (MonoMethod *method, MonoDebugMethodJitInfo *jit, MonoDomain *domain)
{
	MonoDebugDataTable *table;
	MonoDebugMethodAddress *address;
	MonoDebugMethodInfo *minfo;
	MonoDebugHandle *handle;
	guint8 buffer [BUFSIZ];
	guint8 *ptr, *oldptr;
	guint32 i, size, total_size, max_size;

	mono_debugger_lock ();

	table = lookup_data_table (domain);

	handle = mono_debug_get_image (method->klass->image);
	minfo = mono_debug_lookup_method_internal (method);

	max_size = (5 * 5) + 1 + (10 * jit->num_line_numbers) +
		(25 + sizeof (gpointer)) * (1 + jit->num_params + jit->num_locals);

	if (max_size > BUFSIZ)
		ptr = oldptr = g_malloc (max_size);
	else
		ptr = oldptr = buffer;

	write_leb128 (jit->prologue_end, ptr, &ptr);
	write_leb128 (jit->epilogue_begin, ptr, &ptr);

	write_leb128 (jit->num_line_numbers, ptr, &ptr);
	for (i = 0; i < jit->num_line_numbers; i++) {
		MonoDebugLineNumberEntry *lne = &jit->line_numbers [i];

		write_sleb128 (lne->il_offset, ptr, &ptr);
		write_sleb128 (lne->native_offset, ptr, &ptr);
	}

	*ptr++ = jit->this_var ? 1 : 0;
	if (jit->this_var)
		write_variable (jit->this_var, ptr, &ptr);

	write_leb128 (jit->num_params, ptr, &ptr);
	for (i = 0; i < jit->num_params; i++)
		write_variable (&jit->params [i], ptr, &ptr);

	write_leb128 (jit->num_locals, ptr, &ptr);
	for (i = 0; i < jit->num_locals; i++)
		write_variable (&jit->locals [i], ptr, &ptr);

	*ptr++ = jit->gsharedvt_info_var ? 1 : 0;
	if (jit->gsharedvt_info_var) {
		write_variable (jit->gsharedvt_info_var, ptr, &ptr);
		write_variable (jit->gsharedvt_locals_var, ptr, &ptr);
	}

	size = ptr - oldptr;
	g_assert (size < max_size);
	total_size = size + sizeof (MonoDebugMethodAddress);

	if (method_is_dynamic (method)) {
		address = g_malloc0 (total_size);
	} else {
		address = mono_mempool_alloc (table->mp, total_size);
	}

	address->code_start = jit->code_start;
	address->code_size = jit->code_size;

	memcpy (&address->data, oldptr, size);
	if (max_size > BUFSIZ)
		g_free (oldptr);

	g_hash_table_insert (table->method_address_hash, method, address);

	mono_debugger_unlock ();
	return address;
}

void
mono_debug_remove_method (MonoMethod *method, MonoDomain *domain)
{
	MonoDebugDataTable *table;
	MonoDebugMethodAddress *address;

	if (!mono_debug_initialized)
		return;

	g_assert (method_is_dynamic (method));

	mono_debugger_lock ();

	table = lookup_data_table (domain);

	address = g_hash_table_lookup (table->method_address_hash, method);
	if (address)
		g_free (address);

	g_hash_table_remove (table->method_address_hash, method);

	mono_debugger_unlock ();
}

void
mono_debug_add_delegate_trampoline (gpointer code, int size)
{
}

static inline guint32
read_leb128 (guint8 *ptr, guint8 **rptr)
{
	guint32 result = 0, shift = 0;

	while (TRUE) {
		guint8 byte = *ptr++;

		result |= (byte & 0x7f) << shift;
		if ((byte & 0x80) == 0)
			break;
		shift += 7;
	}

	*rptr = ptr;
	return result;
}

static inline gint32
read_sleb128 (guint8 *ptr, guint8 **rptr)
{
	gint32 result = 0;
	guint32 shift = 0;

	while (TRUE) {
		guint8 byte = *ptr++;

		result |= (byte & 0x7f) << shift;
		shift += 7;

		if (byte & 0x80)
			continue;

		if ((shift < 32) && (byte & 0x40))
			result |= - (1 << shift);
		break;
	}

	*rptr = ptr;
	return result;
}

static void
read_variable (MonoDebugVarInfo *var, guint8 *ptr, guint8 **rptr)
{
	var->index = read_leb128 (ptr, &ptr);
	var->offset = read_sleb128 (ptr, &ptr);
	var->size = read_leb128 (ptr, &ptr);
	var->begin_scope = read_leb128 (ptr, &ptr);
	var->end_scope = read_leb128 (ptr, &ptr);
	READ_UNALIGNED (gpointer, ptr, var->type);
	ptr += sizeof (gpointer);
	*rptr = ptr;
}

void
mono_debug_free_method_jit_info (MonoDebugMethodJitInfo *jit)
{
	if (!jit)
		return;
	g_free (jit->line_numbers);
	g_free (jit->this_var);
	g_free (jit->params);
	g_free (jit->locals);
	g_free (jit->gsharedvt_info_var);
	g_free (jit->gsharedvt_locals_var);
	g_free (jit);
}

static MonoDebugMethodJitInfo *
mono_debug_read_method (MonoDebugMethodAddress *address)
{
	MonoDebugMethodJitInfo *jit;
	guint32 i;
	guint8 *ptr;

	jit = g_new0 (MonoDebugMethodJitInfo, 1);
	jit->code_start = address->code_start;
	jit->code_size = address->code_size;

	ptr = (guint8 *) &address->data;

	jit->prologue_end = read_leb128 (ptr, &ptr);
	jit->epilogue_begin = read_leb128 (ptr, &ptr);

	jit->num_line_numbers = read_leb128 (ptr, &ptr);
	jit->line_numbers = g_new0 (MonoDebugLineNumberEntry, jit->num_line_numbers);
	for (i = 0; i < jit->num_line_numbers; i++) {
		MonoDebugLineNumberEntry *lne = &jit->line_numbers [i];

		lne->il_offset = read_sleb128 (ptr, &ptr);
		lne->native_offset = read_sleb128 (ptr, &ptr);
	}

	if (*ptr++) {
		jit->this_var = g_new0 (MonoDebugVarInfo, 1);
		read_variable (jit->this_var, ptr, &ptr);
	}

	jit->num_params = read_leb128 (ptr, &ptr);
	jit->params = g_new0 (MonoDebugVarInfo, jit->num_params);
	for (i = 0; i < jit->num_params; i++)
		read_variable (&jit->params [i], ptr, &ptr);

	jit->num_locals = read_leb128 (ptr, &ptr);
	jit->locals = g_new0 (MonoDebugVarInfo, jit->num_locals);
	for (i = 0; i < jit->num_locals; i++)
		read_variable (&jit->locals [i], ptr, &ptr);

	if (*ptr++) {
		jit->gsharedvt_info_var = g_new0 (MonoDebugVarInfo, 1);
		jit->gsharedvt_locals_var = g_new0 (MonoDebugVarInfo, 1);
		read_variable (jit->gsharedvt_info_var, ptr, &ptr);
		read_variable (jit->gsharedvt_locals_var, ptr, &ptr);
	}

	return jit;
}

static MonoDebugMethodJitInfo *
find_method (MonoMethod *method, MonoDomain *domain)
{
	MonoDebugDataTable *table;
	MonoDebugMethodAddress *address;

	table = lookup_data_table (domain);
	address = g_hash_table_lookup (table->method_address_hash, method);

	if (!address)
		return NULL;

	return mono_debug_read_method (address);
}

MonoDebugMethodJitInfo *
mono_debug_find_method (MonoMethod *method, MonoDomain *domain)
{
	MonoDebugMethodJitInfo *res;

	if (mono_debug_format == MONO_DEBUG_FORMAT_NONE)
		return NULL;

	mono_debugger_lock ();
	res = find_method (method, domain);
	mono_debugger_unlock ();
	return res;
}

MonoDebugMethodAddressList *
mono_debug_lookup_method_addresses (MonoMethod *method)
{
	g_assert_not_reached ();
	return NULL;
}

static gint32
il_offset_from_address (MonoMethod *method, MonoDomain *domain, guint32 native_offset)
{
	MonoDebugMethodJitInfo *jit;
	int i;

	jit = find_method (method, domain);
	if (!jit || !jit->line_numbers)
		goto cleanup_and_fail;

	for (i = jit->num_line_numbers - 1; i >= 0; i--) {
		MonoDebugLineNumberEntry lne = jit->line_numbers [i];

		if (lne.native_offset <= native_offset) {
			mono_debug_free_method_jit_info (jit);
			return lne.il_offset;
		}
	}

cleanup_and_fail:
	mono_debug_free_method_jit_info (jit);
	return -1;
}

/**
 * mono_debug_il_offset_from_address:
 *
 *   Compute the IL offset corresponding to NATIVE_OFFSET inside the native
 * code of METHOD in DOMAIN.
 */
gint32
mono_debug_il_offset_from_address (MonoMethod *method, MonoDomain *domain, guint32 native_offset)
{
	gint32 res;

	mono_debugger_lock ();

	res = il_offset_from_address (method, domain, native_offset);

	mono_debugger_unlock ();

	return res;
}

/**
 * mono_debug_lookup_source_location:
 * @address: Native offset within the @method's machine code.
 *
 * Lookup the source code corresponding to the machine instruction located at
 * native offset @address within @method.
 *
 * The returned `MonoDebugSourceLocation' contains both file / line number
 * information and the corresponding IL offset.  It must be freed by
 * mono_debug_free_source_location().
 */
MonoDebugSourceLocation *
mono_debug_lookup_source_location (MonoMethod *method, guint32 address, MonoDomain *domain)
{
	MonoDebugMethodInfo *minfo;
	MonoDebugSourceLocation *location;
	gint32 offset;

	if (mono_debug_format == MONO_DEBUG_FORMAT_NONE)
		return NULL;

	mono_debugger_lock ();
	minfo = mono_debug_lookup_method_internal (method);
	if (!minfo || !minfo->handle || !minfo->handle->symfile || !mono_debug_symfile_is_loaded (minfo->handle->symfile)) {
		mono_debugger_unlock ();
		return NULL;
	}

	offset = il_offset_from_address (method, domain, address);
	if (offset < 0) {
		mono_debugger_unlock ();
		return NULL;
	}

	location = mono_debug_symfile_lookup_location (minfo, offset);
	mono_debugger_unlock ();
	return location;
}

/*
 * mono_debug_lookup_locals:
 *
 *   Return information about the local variables of MINFO.
 * The result should be freed using mono_debug_symfile_free_locals ().
 */
MonoDebugLocalsInfo*
mono_debug_lookup_locals (MonoMethod *method)
{
	MonoDebugMethodInfo *minfo;
	MonoDebugLocalsInfo *res;

	if (mono_debug_format == MONO_DEBUG_FORMAT_NONE)
		return NULL;

	mono_debugger_lock ();
	minfo = mono_debug_lookup_method_internal (method);
	if (!minfo || !minfo->handle || !minfo->handle->symfile || !mono_debug_symfile_is_loaded (minfo->handle->symfile)) {
		mono_debugger_unlock ();
		return NULL;
	}

	res = mono_debug_symfile_lookup_locals (minfo);
	mono_debugger_unlock ();

	return res;
}

/**
 * mono_debug_free_source_location:
 * @location: A `MonoDebugSourceLocation'.
 *
 * Frees the @location.
 */
void
mono_debug_free_source_location (MonoDebugSourceLocation *location)
{
	if (location) {
		g_free (location->source_file);
		g_free (location);
	}
}

/**
 * mono_debug_print_stack_frame:
 * @native_offset: Native offset within the @method's machine code.
 *
 * Conventient wrapper around mono_debug_lookup_source_location() which can be
 * used if you only want to use the location to print a stack frame.
 */
gchar *
mono_debug_print_stack_frame (MonoMethod *method, guint32 native_offset, MonoDomain *domain)
{
	MonoDebugSourceLocation *location;
	gchar *fname, *ptr, *res;
	int offset;

	fname = mono_method_full_name (method, TRUE);
	for (ptr = fname; *ptr; ptr++) {
		if (*ptr == ':') *ptr = '.';
	}

	location = mono_debug_lookup_source_location (method, native_offset, domain);

	if (!location) {
		if (mono_debug_initialized) {
			mono_debugger_lock ();
			offset = il_offset_from_address (method, domain, native_offset);
			mono_debugger_unlock ();
		} else {
			offset = -1;
		}

		if (offset < 0)
			res = g_strdup_printf ("at %s <0x%05x>", fname, native_offset);
		else
			res = g_strdup_printf ("at %s <IL 0x%05x, 0x%05x>", fname, offset, native_offset);
		g_free (fname);
		return res;
	}

	res = g_strdup_printf ("at %s [0x%05x] in %s:%d", fname, location->il_offset,
			       location->source_file, location->row);

	g_free (fname);
	mono_debug_free_source_location (location);
	return res;
}

void
mono_set_is_debugger_attached (gboolean attached)
{
	is_attached = attached;
}

gboolean
mono_is_debugger_attached (void)
{
	return is_attached;
}

/*
 * Bundles
 */

typedef struct _BundledSymfile BundledSymfile;

struct _BundledSymfile {
	BundledSymfile *next;
	const char *aname;
	const mono_byte *raw_contents;
	int size;
};

static BundledSymfile *bundled_symfiles = NULL;

void
mono_register_symfile_for_assembly (const char *assembly_name, const mono_byte *raw_contents, int size)
{
	BundledSymfile *bsymfile;

	bsymfile = g_new0 (BundledSymfile, 1);
	bsymfile->aname = assembly_name;
	bsymfile->raw_contents = raw_contents;
	bsymfile->size = size;
	bsymfile->next = bundled_symfiles;
	bundled_symfiles = bsymfile;
}

static MonoDebugHandle *
open_symfile_from_bundle (MonoImage *image)
{
	BundledSymfile *bsymfile;

	for (bsymfile = bundled_symfiles; bsymfile; bsymfile = bsymfile->next) {
		if (strcmp (bsymfile->aname, image->module_name))
			continue;

		return mono_debug_open_image (image, bsymfile->raw_contents, bsymfile->size);
	}

	return NULL;
}

void
mono_debugger_lock (void)
{
	g_assert (initialized);
	mono_mutex_lock (&debugger_lock_mutex);
}

void
mono_debugger_unlock (void)
{
	g_assert (initialized);
	mono_mutex_unlock (&debugger_lock_mutex);
}

void
mono_debugger_initialize ()
{
	mono_mutex_init_recursive (&debugger_lock_mutex);
	initialized = 1;
}

/**
 * mono_debug_enabled:
 *
 * Returns true is debug information is enabled. This doesn't relate if a debugger is present or not.
 */
mono_bool
mono_debug_enabled (void)
{
	return mono_debug_format != MONO_DEBUG_FORMAT_NONE;
}
