/***********************************************************************/
/*                                                                     */
/*                                OCaml                                */
/*                                                                     */
/*         Xavier Leroy and Damien Doligez, INRIA Rocquencourt         */
/*                                                                     */
/*  Copyright 1996 Institut National de Recherche en Informatique et   */
/*  en Automatique.  All rights reserved.  This file is distributed    */
/*  under the terms of the GNU Library General Public License, with    */
/*  the special exception on linking described in file ../LICENSE.     */
/*                                                                     */
/***********************************************************************/

/* Start-up code */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include "config.h"
#ifdef HAS_UNISTD
#include <unistd.h>
#endif
#ifdef _WIN32
#include <process.h>
#endif
#include "alloc.h"
#include "backtrace.h"
#include "callback.h"
#include "custom.h"
#include "debugger.h"
#include "dynlink.h"
#include "eventlog.h"
#include "exec.h"
#include "fail.h"
#include "fix_code.h"
#include "gc_ctrl.h"
#include "instrtrace.h"
#include "interp.h"
#include "intext.h"
#include "io.h"
#include "memory.h"
#include "minor_gc.h"
#include "misc.h"
#include "mlvalues.h"
#include "osdeps.h"
#include "prims.h"
#include "printexc.h"
#include "reverse.h"
#include "signals.h"
#include "fiber.h"
#include "sys.h"
#include "startup.h"
#include "version.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef SEEK_END
#define SEEK_END 2
#endif

unsigned int *caml_alloc_counts = NULL;
unsigned int *caml_time_counts = NULL;

/* Read the trailer of a bytecode file */

static void fixup_endianness_trailer(uint32 * p)
{
#ifndef ARCH_BIG_ENDIAN
  Reverse_32(p, p);
#endif
}

static int read_trailer(int fd, struct exec_trailer *trail)
{
  if (lseek(fd, (long) -TRAILER_SIZE, SEEK_END) == -1)
    return BAD_BYTECODE;
  if (read(fd, (char *) trail, TRAILER_SIZE) < TRAILER_SIZE)
    return BAD_BYTECODE;
  fixup_endianness_trailer(&trail->num_sections);
  if (strncmp(trail->magic, EXEC_MAGIC, 12) == 0)
    return 0;
  else
    return BAD_BYTECODE;
}

int caml_attempt_open(char **name, struct exec_trailer *trail,
                      int do_open_script)
{
  char * truename;
  int fd;
  int err;
  char buf [2];

  truename = caml_search_exe_in_path(*name);
  *name = truename;
  caml_gc_log("Opening bytecode executable %s", truename);
  fd = open(truename, O_RDONLY | O_BINARY);
  if (fd == -1) {
    caml_gc_log("Cannot open file");
    return FILE_NOT_FOUND;
  }
  if (!do_open_script) {
    err = read (fd, buf, 2);
    if (err < 2 || (buf [0] == '#' && buf [1] == '!')) {
      close(fd);
      caml_gc_log("Rejected #! script");
      return BAD_BYTECODE;
    }
  }
  err = read_trailer(fd, trail);
  if (err != 0) {
    close(fd);
    caml_gc_log("Not a bytecode executable");
    return err;
  }
  return fd;
}

/* Read the section descriptors */

void caml_read_section_descriptors(int fd, struct exec_trailer *trail)
{
  int toc_size, i;

  toc_size = trail->num_sections * 8;
  trail->section = caml_stat_alloc(toc_size);
  lseek(fd, - (long) (TRAILER_SIZE + toc_size), SEEK_END);
  if (read(fd, (char *) trail->section, toc_size) != toc_size)
    caml_fatal_error("Fatal error: cannot read section table\n");
  /* Fixup endianness of lengths */
  for (i = 0; i < trail->num_sections; i++)
    fixup_endianness_trailer(&(trail->section[i].len));
}

/* Position fd at the beginning of the section having the given name.
   Return the length of the section data in bytes, or -1 if no section
   found with that name. */

int32 caml_seek_optional_section(int fd, struct exec_trailer *trail, char *name)
{
  long ofs;
  int i;

  ofs = TRAILER_SIZE + trail->num_sections * 8;
  for (i = trail->num_sections - 1; i >= 0; i--) {
    ofs += trail->section[i].len;
    if (strncmp(trail->section[i].name, name, 4) == 0) {
      lseek(fd, -ofs, SEEK_END);
      return trail->section[i].len;
    }
  }
  return -1;
}

/* Position fd at the beginning of the section having the given name.
   Return the length of the section data in bytes. */

int32 caml_seek_section(int fd, struct exec_trailer *trail, char *name)
{
  int32 len = caml_seek_optional_section(fd, trail, name);
  if (len == -1)
    caml_fatal_error_arg("Fatal_error: section `%s' is missing\n", name);
  return len;
}

/* Read and return the contents of the section having the given name.
   Add a terminating 0.  Return NULL if no such section. */

static char * read_section(int fd, struct exec_trailer *trail, char *name)
{
  int32 len;
  char * data;

  len = caml_seek_optional_section(fd, trail, name);
  if (len == -1) return NULL;
  data = caml_stat_alloc(len + 1);
  if (read(fd, data, len) != len)
    caml_fatal_error_arg("Fatal error: error reading section %s\n", name);
  data[len] = 0;
  return data;
}

/* Invocation of ocamlrun: 4 cases.

   1.  runtime + bytecode
       user types:  ocamlrun [options] bytecode args...
       arguments:  ocamlrun [options] bytecode args...

   2.  bytecode script
       user types:  bytecode args...
   2a  (kernel 1) arguments:  ocamlrun ./bytecode args...
   2b  (kernel 2) arguments:  bytecode bytecode args...

   3.  concatenated runtime and bytecode
       user types:  composite args...
       arguments:  composite args...

Algorithm:
  1-  If argument 0 is a valid byte-code file that does not start with #!,
      then we are in case 3 and we pass the same command line to the
      OCaml program.
  2-  In all other cases, we parse the command line as:
        (whatever) [options] bytecode args...
      and we strip "(whatever) [options]" from the command line.

*/

extern void caml_init_ieee_floats (void);

#ifdef _WIN32
extern void caml_signal_thread(void * lpParam);
#endif

#ifdef _MSC_VER

/* PR 4887: avoid crash box of windows runtime on some system calls */
extern void caml_install_invalid_parameter_handler();

#endif

struct sigaction sa;
struct itimerval timer;
extern code_t pc;

static void timer_handler (int signum)
{
  if (caml_time_counts && pc && pc - caml_start_code < caml_code_size)
    caml_time_counts[(long)(pc - caml_start_code)]++;
}

/* Main entry point when loading code from a file */

CAMLexport void caml_main(char **argv)
{
  int fd, pos;
  struct exec_trailer trail;
  struct channel * chan;
  value res;
  char * shared_lib_path, * shared_libs, * req_prims;
  char * exe_name;
  static char proc_self_exe[256];

  caml_init_startup_params();
  /* Machine-dependent initialization of the floating-point hardware
     so that it behaves as much as possible as specified in IEEE */
  caml_init_ieee_floats();
#ifdef _MSC_VER
  caml_install_invalid_parameter_handler();
#endif
  caml_init_custom_operations();
  caml_ext_table_init(&caml_shared_libs_path, 8);
  caml_external_raise = NULL;
  /* Determine options and position of bytecode file */
  pos = 0;

  /* First, try argv[0] (when ocamlrun is called by a bytecode program) */
  exe_name = argv[0];
  fd = caml_attempt_open(&exe_name, &trail, 0);

  /* Should we really do that at all?  The current executable is ocamlrun
     itself, it's never a bytecode program. */
  if (fd < 0
      && caml_executable_name(proc_self_exe, sizeof(proc_self_exe)) == 0) {
    exe_name = proc_self_exe;
    fd = caml_attempt_open(&exe_name, &trail, 0);
  }

  if (fd < 0) {
    pos = caml_parse_command_line(argv);
    if (argv[pos] == 0)
      caml_fatal_error("No bytecode file specified.\n");
    exe_name = argv[pos];
    fd = caml_attempt_open(&exe_name, &trail, 1);
    switch(fd) {
    case FILE_NOT_FOUND:
      caml_fatal_error_arg("Fatal error: cannot find file '%s'\n", argv[pos]);
      break;
    case BAD_BYTECODE:
      caml_fatal_error_arg(
        "Fatal error: the file '%s' is not a bytecode executable file\n",
        exe_name);
      break;
    }
  }
  caml_startup_params.exe_name = exe_name;
  caml_startup_params.main_argv = argv + pos;

  /* Read the table of contents (section descriptors) */
  caml_read_section_descriptors(fd, &trail);
  /* Initialize the abstract machine */
  caml_init_gc ();
  if (caml_startup_params.backtrace_enabled_init) caml_record_backtrace(Val_int(1));
  if (caml_startup_params.eventlog_enabled) caml_setup_eventlog();
  /* Initialize the interpreter */
  caml_interprete(NULL, 0);
  /* Initialize the debugger, if needed */
  caml_debugger_init();
  /* Load the code */
  caml_code_size = caml_seek_section(fd, &trail, "CODE");
  caml_load_code(fd, caml_code_size);
  /* Initialize the profiler */
  char* prof_alloc_file = getenv("CAML_PROFILE_ALLOC");
  if (prof_alloc_file != NULL) {
    caml_alloc_counts =
      (unsigned int *) caml_stat_alloc (caml_code_size * sizeof(unsigned int));
    for (int i = 0; i < caml_code_size; i++) caml_alloc_counts[i] = 0;
  }
  char* prof_time_file = getenv("CAML_PROFILE_TIME");
  if (prof_time_file != NULL) {
    caml_time_counts =
      (unsigned int *) caml_stat_alloc (caml_code_size * sizeof(unsigned int));
    for (int i = 0; i < caml_code_size; i++) caml_time_counts[i] = 0;
    /* Install timer_handler as the signal handler for SIGPROF. */
    memset (&sa, 0, sizeof (sa));
    sa.sa_handler = &timer_handler;
    sigaction (SIGPROF, &sa, NULL);

    /* Configure the timer to expire after 10 msec... */
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 10000;
    /* ... and every 10 msec after that. */
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 10000;
    /* Start a profiling timer. It counts down whenever this process is
      executing. */
    setitimer (ITIMER_PROF, &timer, NULL);
  }
  /* Build the table of primitives */
  shared_lib_path = read_section(fd, &trail, "DLPT");
  shared_libs = read_section(fd, &trail, "DLLS");
  req_prims = read_section(fd, &trail, "PRIM");
  if (req_prims == NULL) caml_fatal_error("Fatal error: no PRIM section\n");
  caml_build_primitive_table(shared_lib_path, shared_libs, req_prims);
  caml_stat_free(shared_lib_path);
  caml_stat_free(shared_libs);
  caml_stat_free(req_prims);
  /* Load the globals */
  caml_seek_section(fd, &trail, "DATA");
  chan = caml_open_descriptor_in(fd);
  caml_modify_root(caml_global_data, caml_input_val(chan));
  caml_close_channel(chan); /* this also closes fd */
  caml_stat_free(trail.section);
#ifdef _WIN32
  /* Start a thread to handle signals */
  if (getenv("CAMLSIGPIPE"))
    _beginthread(caml_signal_thread, 4096, NULL);
#endif
  /* Execute the program */
  caml_debugger(PROGRAM_START);
  res = caml_interprete(caml_start_code, caml_code_size);
  /* Output profile */
  if (prof_alloc_file != NULL) {
    FILE* fp = fopen (prof_alloc_file, "w");
    for (int i = 0; i < caml_code_size; i++) {
      if (caml_alloc_counts[i] != 0)
        fprintf (fp, "%d\t%u\n", i,caml_alloc_counts[i]);
    }
    fflush(fp);
    fclose(fp);
  }
  if (prof_time_file != NULL) {
    FILE* fp = fopen (prof_time_file, "w");
    for (int i = 0; i < caml_code_size; i++) {
      if (caml_time_counts[i] != 0)
        fprintf (fp, "%d\t%u\n", i,caml_time_counts[i]);
    }
    fflush(fp);
    fclose(fp);
  }
  if (Is_exception_result(res)) {
    caml_exn_bucket = Extract_exception(res);
    if (caml_debugger_in_use) {
      caml_extern_sp = &caml_exn_bucket; /* The debugger needs the
                                            exception value.*/
      caml_debugger(UNCAUGHT_EXC);
    }
    caml_fatal_uncaught_exception(caml_exn_bucket);
  }
}

/* Main entry point when code is linked in as initialized data */

CAMLexport void caml_startup_code(
           code_t code, asize_t code_size,
           char *data, asize_t data_size,
           char *section_table, asize_t section_table_size,
           char **argv)
{
  value res;
  char * cds_file;
  char * exe_name;
  static char proc_self_exe[256];

  caml_init_startup_params();
  caml_init_ieee_floats();
#ifdef _MSC_VER
  caml_install_invalid_parameter_handler();
#endif
  caml_init_custom_operations();
#ifdef DEBUG
  caml_startup_params.verb_gc = 63;
#endif
  cds_file = getenv("CAML_DEBUG_FILE");
  if (cds_file != NULL) {
    caml_cds_file = caml_strdup(cds_file);
  }
  exe_name = argv[0];
  if (caml_executable_name(proc_self_exe, sizeof(proc_self_exe)) == 0)
    exe_name = proc_self_exe;
  caml_external_raise = NULL;
  caml_startup_params.exe_name = exe_name;
  caml_startup_params.main_argv = argv;
  /* Initialize the abstract machine */
  caml_init_gc ();
  if (caml_startup_params.backtrace_enabled_init) caml_record_backtrace(Val_int(1));
  /* Initialize the interpreter */
  caml_interprete(NULL, 0);
  /* Initialize the debugger, if needed */
  caml_debugger_init();
  /* Load the code */
  caml_start_code = code;
  caml_code_size = code_size;
  caml_init_code_fragments();
  if (caml_debugger_in_use) {
    int len, i;
    len = code_size / sizeof(opcode_t);
    caml_saved_code = (unsigned char *) caml_stat_alloc(len);
    for (i = 0; i < len; i++) caml_saved_code[i] = caml_start_code[i];
  }
#ifdef THREADED_CODE
  caml_thread_code(caml_start_code, code_size);
#endif
  /* Use the builtin table of primitives */
  caml_build_primitive_table_builtin();
  /* Load the globals */
  caml_modify_root(caml_global_data, caml_input_value_from_block(data, data_size));
  /* Record the sections (for caml_get_section_table in meta.c) */
  caml_section_table = section_table;
  caml_section_table_size = section_table_size;
  /* Execute the program */
  caml_debugger(PROGRAM_START);
  res = caml_interprete(caml_start_code, caml_code_size);
  if (Is_exception_result(res)) {
    caml_exn_bucket = Extract_exception(res);
    if (caml_debugger_in_use) {
      caml_extern_sp = &caml_exn_bucket; /* The debugger needs the
                                            exception value.*/
      caml_debugger(UNCAUGHT_EXC);
    }
    caml_fatal_uncaught_exception(caml_exn_bucket);
  }
}
