/*
Copyright (c) 2013. The YARA Authors. All Rights Reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <yara/libyara.h>
#include <yara/utils.h>
#include <yara/compiler.h>
#include <yara/exec.h>
#include <yara/error.h>
#include <yara/mem.h>
#include <yara/object.h>
#include <yara/lexer.h>
#include <yara/strutils.h>


YR_API int yr_compiler_create(
    YR_COMPILER** compiler)
{
  int result;
  YR_COMPILER* new_compiler;

  new_compiler = (YR_COMPILER*) yr_calloc(1, sizeof(YR_COMPILER));

  if (new_compiler == NULL)
    return ERROR_INSUFFICIENT_MEMORY;

  new_compiler->errors = 0;
  new_compiler->callback = NULL;
  new_compiler->last_error = ERROR_SUCCESS;
  new_compiler->last_error_line = 0;
  new_compiler->current_line = 0;
  new_compiler->last_result = ERROR_SUCCESS;
  new_compiler->file_stack_ptr = 0;
  new_compiler->file_name_stack_ptr = 0;
  new_compiler->fixup_stack_head = NULL;
  new_compiler->allow_includes = 1;
  new_compiler->loop_depth = 0;
  new_compiler->loop_for_of_mem_offset = -1;
  new_compiler->compiled_rules_arena = NULL;
  new_compiler->namespaces_count = 0;
  new_compiler->current_rule = NULL;

  result = yr_hash_table_create(10007, &new_compiler->rules_table);

  if (result == ERROR_SUCCESS)
    result = yr_hash_table_create(10007, &new_compiler->objects_table);

  if (result == ERROR_SUCCESS)
    result = yr_hash_table_create(101, &new_compiler->strings_table);

  if (result == ERROR_SUCCESS)
    result = yr_arena_create(65536, 0, &new_compiler->sz_arena);

  if (result == ERROR_SUCCESS)
    result = yr_arena_create(65536, 0, &new_compiler->rules_arena);

  if (result == ERROR_SUCCESS)
    result = yr_arena_create(65536, 0, &new_compiler->strings_arena);

  if (result == ERROR_SUCCESS)
      result = yr_arena_create(65536, 0, &new_compiler->code_arena);

  if (result == ERROR_SUCCESS)
    result = yr_arena_create(65536, 0, &new_compiler->re_code_arena);

  if (result == ERROR_SUCCESS)
    result = yr_arena_create(65536, 0, &new_compiler->externals_arena);

  if (result == ERROR_SUCCESS)
    result = yr_arena_create(65536, 0, &new_compiler->namespaces_arena);

  if (result == ERROR_SUCCESS)
    result = yr_arena_create(65536, 0, &new_compiler->metas_arena);

  if (result == ERROR_SUCCESS)
    result = yr_arena_create(65536, 0, &new_compiler->automaton_arena);

  if (result == ERROR_SUCCESS)
    result = yr_arena_create(65536, 0, &new_compiler->matches_arena);

  if (result == ERROR_SUCCESS)
    result = yr_ac_automaton_create(&new_compiler->automaton);

  if (result == ERROR_SUCCESS)
  {
    *compiler = new_compiler;
  }
  else  // if error, do cleanup
  {
    yr_compiler_destroy(new_compiler);
  }

  return result;
}


YR_API void yr_compiler_destroy(
    YR_COMPILER* compiler)
{
  YR_FIXUP* fixup;
  int i;

  yr_arena_destroy(compiler->compiled_rules_arena);
  yr_arena_destroy(compiler->sz_arena);
  yr_arena_destroy(compiler->rules_arena);
  yr_arena_destroy(compiler->strings_arena);
  yr_arena_destroy(compiler->code_arena);
  yr_arena_destroy(compiler->re_code_arena);
  yr_arena_destroy(compiler->externals_arena);
  yr_arena_destroy(compiler->namespaces_arena);
  yr_arena_destroy(compiler->metas_arena);
  yr_arena_destroy(compiler->automaton_arena);
  yr_arena_destroy(compiler->matches_arena);

  yr_ac_automaton_destroy(compiler->automaton);

  yr_hash_table_destroy(
      compiler->rules_table,
      NULL);

  yr_hash_table_destroy(
      compiler->strings_table,
      NULL);

  yr_hash_table_destroy(
      compiler->objects_table,
      (YR_HASH_TABLE_FREE_VALUE_FUNC) yr_object_destroy);

  for (i = 0; i < compiler->file_name_stack_ptr; i++)
    yr_free(compiler->file_name_stack[i]);

  fixup = compiler->fixup_stack_head;

  while (fixup != NULL)
  {
    YR_FIXUP* next_fixup = fixup->next;
    yr_free(fixup);
    fixup = next_fixup;
  }

  yr_free(compiler);
}


YR_API void yr_compiler_set_callback(
    YR_COMPILER* compiler,
    YR_COMPILER_CALLBACK_FUNC callback,
    void* user_data)
{
  compiler->callback = callback;
  compiler->user_data = user_data;
}


int _yr_compiler_push_file(
    YR_COMPILER* compiler,
    FILE* fh)
{
  if (compiler->file_stack_ptr < MAX_INCLUDE_DEPTH)
  {
    compiler->file_stack[compiler->file_stack_ptr] = fh;
    compiler->file_stack_ptr++;
    return ERROR_SUCCESS;
  }
  else
  {
    compiler->last_result = ERROR_INCLUDE_DEPTH_EXCEEDED;
    return ERROR_INCLUDE_DEPTH_EXCEEDED;
  }
}


FILE* _yr_compiler_pop_file(
    YR_COMPILER* compiler)
{
  FILE* result = NULL;

  if (compiler->file_stack_ptr > 0)
  {
    compiler->file_stack_ptr--;
    result = compiler->file_stack[compiler->file_stack_ptr];
  }

  return result;
}

int _yr_compiler_push_file_name(
    YR_COMPILER* compiler,
    const char* file_name)
{
  char* str;
  int i;

  for (i = 0; i < compiler->file_name_stack_ptr; i++)
  {
    if (strcmp(file_name, compiler->file_name_stack[i]) == 0)
    {
      compiler->last_result = ERROR_INCLUDES_CIRCULAR_REFERENCE;
      return ERROR_INCLUDES_CIRCULAR_REFERENCE;
    }
  }

  if (compiler->file_name_stack_ptr < MAX_INCLUDE_DEPTH)
  {
    str = yr_strdup(file_name);

    if (str == NULL)
      return ERROR_INSUFFICIENT_MEMORY;

    compiler->file_name_stack[compiler->file_name_stack_ptr] = str;
    compiler->file_name_stack_ptr++;

    return ERROR_SUCCESS;
  }
  else
  {
    compiler->last_result = ERROR_INCLUDE_DEPTH_EXCEEDED;
    return ERROR_INCLUDE_DEPTH_EXCEEDED;
  }
}


void _yr_compiler_pop_file_name(
    YR_COMPILER* compiler)
{
  if (compiler->file_name_stack_ptr > 0)
  {
    compiler->file_name_stack_ptr--;
    yr_free(compiler->file_name_stack[compiler->file_name_stack_ptr]);
    compiler->file_name_stack[compiler->file_name_stack_ptr] = NULL;
  }
}


YR_API char* yr_compiler_get_current_file_name(
    YR_COMPILER* context)
{
  if (context->file_name_stack_ptr > 0)
  {
    return context->file_name_stack[context->file_name_stack_ptr - 1];
  }
  else
  {
    return NULL;
  }
}


int _yr_compiler_set_namespace(
    YR_COMPILER* compiler,
    const char* namespace_)
{
  YR_NAMESPACE* ns;

  char* ns_name;
  int result;
  int i;
  int found;

  ns = (YR_NAMESPACE*) yr_arena_base_address(compiler->namespaces_arena);
  found = FALSE;

  for (i = 0; i < compiler->namespaces_count; i++)
  {
    if (strcmp(ns->name, namespace_) == 0)
    {
      found = TRUE;
      break;
    }

    ns = (YR_NAMESPACE*) yr_arena_next_address(
        compiler->namespaces_arena,
        ns,
        sizeof(YR_NAMESPACE));
  }

  if (!found)
  {
    result = yr_arena_write_string(
        compiler->sz_arena,
        namespace_,
        &ns_name);

    if (result == ERROR_SUCCESS)
      result = yr_arena_allocate_struct(
          compiler->namespaces_arena,
          sizeof(YR_NAMESPACE),
          (void**) &ns,
          offsetof(YR_NAMESPACE, name),
          EOL);

    if (result != ERROR_SUCCESS)
      return result;

    ns->name = ns_name;

    for (i = 0; i < MAX_THREADS; i++)
      ns->t_flags[i] = 0;

    compiler->namespaces_count++;
  }

  compiler->current_namespace = ns;
  return ERROR_SUCCESS;
}

YR_API int yr_compiler_add_file(
    YR_COMPILER* compiler,
    FILE* rules_file,
    const char* namespace_,
    const char* file_name)
{
  // Don't allow yr_compiler_add_file() after
  // yr_compiler_get_rules() has been called.

  assert(compiler->compiled_rules_arena == NULL);

  if (file_name != NULL)
    _yr_compiler_push_file_name(compiler, file_name);

  if (namespace_ != NULL)
    compiler->last_result = _yr_compiler_set_namespace(compiler, namespace_);
  else
    compiler->last_result = _yr_compiler_set_namespace(compiler, "default");

  if (compiler->last_result == ERROR_SUCCESS)
  {
    return yr_lex_parse_rules_file(rules_file, compiler);
  }
  else
  {
    compiler->errors++;
    return compiler->errors;
  }

}


YR_API int yr_compiler_add_fd(
    YR_COMPILER* compiler,
    YR_FILE_DESCRIPTOR rules_fd,
    const char* namespace_,
    const char* file_name)
{
  // Don't allow yr_compiler_add_fd() after
  // yr_compiler_get_rules() has been called.

  assert(compiler->compiled_rules_arena == NULL);

  if (file_name != NULL)
    _yr_compiler_push_file_name(compiler, file_name);

  if (namespace_ != NULL)
    compiler->last_result = _yr_compiler_set_namespace(compiler, namespace_);
  else
    compiler->last_result = _yr_compiler_set_namespace(compiler, "default");

  if (compiler->last_result == ERROR_SUCCESS)
  {
    return yr_lex_parse_rules_fd(rules_fd, compiler);
  }
  else
  {
    compiler->errors++;
    return compiler->errors;
  }
}


YR_API int yr_compiler_add_string(
    YR_COMPILER* compiler,
    const char* rules_string,
    const char* namespace_)
{
  // Don't allow yr_compiler_add_string() after
  // yr_compiler_get_rules() has been called.

  assert(compiler->compiled_rules_arena == NULL);

  if (namespace_ != NULL)
    compiler->last_result = _yr_compiler_set_namespace(compiler, namespace_);
  else
    compiler->last_result = _yr_compiler_set_namespace(compiler, "default");

  if (compiler->last_result == ERROR_SUCCESS)
  {
    return yr_lex_parse_rules_string(rules_string, compiler);
  }
  else
  {
    compiler->errors++;
    return compiler->errors;
  }
}


int _yr_compiler_compile_rules(
  YR_COMPILER* compiler)
{
  YARA_RULES_FILE_HEADER* rules_file_header = NULL;
  YR_ARENA* arena = NULL;
  YR_RULE null_rule;
  YR_EXTERNAL_VARIABLE null_external;
  YR_AC_TABLES tables;

  uint8_t halt = OP_HALT;
  int result;

  // Write halt instruction at the end of code.
  yr_arena_write_data(
      compiler->code_arena,
      &halt,
      sizeof(uint8_t),
      NULL);

  // Write a null rule indicating the end.
  memset(&null_rule, 0xFA, sizeof(YR_RULE));
  null_rule.g_flags = RULE_GFLAGS_NULL;

  yr_arena_write_data(
      compiler->rules_arena,
      &null_rule,
      sizeof(YR_RULE),
      NULL);

  // Write a null external the end.
  memset(&null_external, 0xFA, sizeof(YR_EXTERNAL_VARIABLE));
  null_external.type = EXTERNAL_VARIABLE_TYPE_NULL;

  yr_arena_write_data(
      compiler->externals_arena,
      &null_external,
      sizeof(YR_EXTERNAL_VARIABLE),
      NULL);

  // Write Aho-Corasick automaton to arena.
  result = yr_ac_compile(
      compiler->automaton,
      compiler->automaton_arena,
      &tables);

  if (result == ERROR_SUCCESS)
    result = yr_arena_create(1024, 0, &arena);

  if (result == ERROR_SUCCESS)
    result = yr_arena_allocate_struct(
        arena,
        sizeof(YARA_RULES_FILE_HEADER),
        (void**) &rules_file_header,
        offsetof(YARA_RULES_FILE_HEADER, rules_list_head),
        offsetof(YARA_RULES_FILE_HEADER, externals_list_head),
        offsetof(YARA_RULES_FILE_HEADER, code_start),
        offsetof(YARA_RULES_FILE_HEADER, match_table),
        offsetof(YARA_RULES_FILE_HEADER, transition_table),
        EOL);

  if (result == ERROR_SUCCESS)
  {
    rules_file_header->rules_list_head = (YR_RULE*) yr_arena_base_address(
        compiler->rules_arena);

    rules_file_header->externals_list_head = (YR_EXTERNAL_VARIABLE*)
		yr_arena_base_address(compiler->externals_arena);

    rules_file_header->code_start = (uint8_t*) yr_arena_base_address(
        compiler->code_arena);

    rules_file_header->match_table = tables.matches;
    rules_file_header->transition_table = tables.transitions;
  }

  if (result == ERROR_SUCCESS)
  {
    result = yr_arena_append(
        arena,
        compiler->code_arena);
  }

  if (result == ERROR_SUCCESS)
  {
    compiler->code_arena = NULL;
    result = yr_arena_append(
        arena,
        compiler->re_code_arena);
  }

  if (result == ERROR_SUCCESS)
  {
    compiler->re_code_arena = NULL;
    result = yr_arena_append(
        arena,
        compiler->rules_arena);
  }

  if (result == ERROR_SUCCESS)
  {
    compiler->rules_arena = NULL;
    result = yr_arena_append(
        arena,
        compiler->strings_arena);
  }

  if (result == ERROR_SUCCESS)
  {
    compiler->strings_arena = NULL;
    result = yr_arena_append(
        arena,
        compiler->externals_arena);
  }

  if (result == ERROR_SUCCESS)
  {
    compiler->externals_arena = NULL;
    result = yr_arena_append(
        arena,
        compiler->namespaces_arena);
  }

  if (result == ERROR_SUCCESS)
  {
    compiler->namespaces_arena = NULL;
    result = yr_arena_append(
        arena,
        compiler->metas_arena);
  }

  if (result == ERROR_SUCCESS)
  {
    compiler->metas_arena = NULL;
    result = yr_arena_append(
        arena,
        compiler->sz_arena);
  }

  if (result == ERROR_SUCCESS)
  {
    compiler->sz_arena = NULL;
    result = yr_arena_append(
        arena,
        compiler->automaton_arena);
  }

  if (result == ERROR_SUCCESS)
  {
    compiler->automaton_arena = NULL;
    result = yr_arena_append(
        arena,
        compiler->matches_arena);
  }

  if (result == ERROR_SUCCESS)
  {
    compiler->matches_arena = NULL;
    compiler->compiled_rules_arena = arena;
    result = yr_arena_coalesce(arena);
  }
  else
  {
    yr_arena_destroy(arena);
  }

  return result;
}


YR_API int yr_compiler_get_rules(
    YR_COMPILER* compiler,
    YR_RULES** rules)
{
  YR_RULES* yara_rules;
  YARA_RULES_FILE_HEADER* rules_file_header;

  *rules = NULL;

  if (compiler->compiled_rules_arena == NULL)
     FAIL_ON_ERROR(_yr_compiler_compile_rules(compiler));

  yara_rules = (YR_RULES*) yr_malloc(sizeof(YR_RULES));

  if (yara_rules == NULL)
    return ERROR_INSUFFICIENT_MEMORY;

  FAIL_ON_ERROR_WITH_CLEANUP(
      yr_arena_duplicate(compiler->compiled_rules_arena, &yara_rules->arena),
      yr_free(yara_rules));

  rules_file_header = (YARA_RULES_FILE_HEADER*) yr_arena_base_address(
      yara_rules->arena);

  yara_rules->externals_list_head = rules_file_header->externals_list_head;
  yara_rules->rules_list_head = rules_file_header->rules_list_head;
  yara_rules->match_table = rules_file_header->match_table;
  yara_rules->transition_table = rules_file_header->transition_table;
  yara_rules->code_start = rules_file_header->code_start;
  memset(yara_rules->tidx_mask, 0, sizeof(yara_rules->tidx_mask));

  FAIL_ON_ERROR_WITH_CLEANUP(
      yr_mutex_create(&yara_rules->mutex),
      // cleanup
      yr_arena_destroy(yara_rules->arena);
      yr_free(yara_rules));

  *rules = yara_rules;

  return ERROR_SUCCESS;
}


YR_API int yr_compiler_define_integer_variable(
    YR_COMPILER* compiler,
    const char* identifier,
    int64_t value)
{
  YR_EXTERNAL_VARIABLE* external;
  YR_OBJECT* object;

  char* id;

  compiler->last_result = ERROR_SUCCESS;

  FAIL_ON_COMPILER_ERROR(yr_arena_write_string(
      compiler->sz_arena,
      identifier,
      &id));

  FAIL_ON_COMPILER_ERROR(yr_arena_allocate_struct(
      compiler->externals_arena,
      sizeof(YR_EXTERNAL_VARIABLE),
      (void**) &external,
      offsetof(YR_EXTERNAL_VARIABLE, identifier),
      EOL));

  external->type = EXTERNAL_VARIABLE_TYPE_INTEGER;
  external->identifier = id;
  external->value.i = value;

  FAIL_ON_COMPILER_ERROR(yr_object_from_external_variable(
      external,
      &object));

  FAIL_ON_COMPILER_ERROR(yr_hash_table_add(
      compiler->objects_table,
      external->identifier,
      NULL,
      (void*) object));

  return ERROR_SUCCESS;
}


YR_API int yr_compiler_define_boolean_variable(
    YR_COMPILER* compiler,
    const char* identifier,
    int value)
{
  YR_EXTERNAL_VARIABLE* external;
  YR_OBJECT* object;

  char* id;

  compiler->last_result = ERROR_SUCCESS;

  FAIL_ON_COMPILER_ERROR(yr_arena_write_string(
      compiler->sz_arena,
      identifier,
      &id));

  FAIL_ON_COMPILER_ERROR(yr_arena_allocate_struct(
      compiler->externals_arena,
      sizeof(YR_EXTERNAL_VARIABLE),
      (void**) &external,
      offsetof(YR_EXTERNAL_VARIABLE, identifier),
      EOL));

  external->type = EXTERNAL_VARIABLE_TYPE_BOOLEAN;
  external->identifier = id;
  external->value.i = value;

  FAIL_ON_COMPILER_ERROR(yr_object_from_external_variable(
      external,
      &object));

  FAIL_ON_COMPILER_ERROR(yr_hash_table_add(
      compiler->objects_table,
      external->identifier,
      NULL,
      (void*) object));

  return ERROR_SUCCESS;
}


YR_API int yr_compiler_define_float_variable(
    YR_COMPILER* compiler,
    const char* identifier,
    double value)
{
  YR_EXTERNAL_VARIABLE* external;
  YR_OBJECT* object;

  char* id;

  compiler->last_result = ERROR_SUCCESS;

  FAIL_ON_COMPILER_ERROR(yr_arena_write_string(
      compiler->sz_arena,
      identifier,
      &id));

  FAIL_ON_COMPILER_ERROR(yr_arena_allocate_struct(
      compiler->externals_arena,
      sizeof(YR_EXTERNAL_VARIABLE),
      (void**) &external,
      offsetof(YR_EXTERNAL_VARIABLE, identifier),
      EOL));

  external->type = EXTERNAL_VARIABLE_TYPE_FLOAT;
  external->identifier = id;
  external->value.f = value;

  FAIL_ON_COMPILER_ERROR(yr_object_from_external_variable(
      external,
      &object));

  FAIL_ON_COMPILER_ERROR(yr_hash_table_add(
      compiler->objects_table,
      external->identifier,
      NULL,
      (void*) object));

  return ERROR_SUCCESS;
}


YR_API int yr_compiler_define_string_variable(
    YR_COMPILER* compiler,
    const char* identifier,
    const char* value)
{
  YR_OBJECT* object;
  YR_EXTERNAL_VARIABLE* external;

  char* id = NULL;
  char* val = NULL;

  compiler->last_result = ERROR_SUCCESS;

  FAIL_ON_COMPILER_ERROR(yr_arena_write_string(
      compiler->sz_arena,
      identifier,
      &id));

  FAIL_ON_COMPILER_ERROR(yr_arena_write_string(
      compiler->sz_arena,
      value,
      &val));

  FAIL_ON_COMPILER_ERROR(yr_arena_allocate_struct(
      compiler->externals_arena,
      sizeof(YR_EXTERNAL_VARIABLE),
      (void**) &external,
      offsetof(YR_EXTERNAL_VARIABLE, identifier),
      offsetof(YR_EXTERNAL_VARIABLE, value.s),
      EOL));

  external->type = EXTERNAL_VARIABLE_TYPE_STRING;
  external->identifier = id;
  external->value.s = val;

  FAIL_ON_COMPILER_ERROR(yr_object_from_external_variable(
      external,
      &object));

  FAIL_ON_COMPILER_ERROR(yr_hash_table_add(
      compiler->objects_table,
      external->identifier,
      NULL,
      (void*) object));

  return compiler->last_result;
}


YR_API char* yr_compiler_get_error_message(
    YR_COMPILER* compiler,
    char* buffer,
    int buffer_size)
{
  uint32_t max_strings_per_rule;

  switch(compiler->last_error)
  {
    case ERROR_INSUFFICIENT_MEMORY:
      snprintf(buffer, buffer_size, "not enough memory");
      break;
    case ERROR_DUPLICATED_IDENTIFIER:
      snprintf(
          buffer,
          buffer_size,
          "duplicated identifier \"%s\"",
          compiler->last_error_extra_info);
      break;
    case ERROR_DUPLICATED_STRING_IDENTIFIER:
      snprintf(
          buffer,
          buffer_size,
          "duplicated string identifier \"%s\"",
          compiler->last_error_extra_info);
      break;
    case ERROR_DUPLICATED_TAG_IDENTIFIER:
      snprintf(
          buffer,
          buffer_size,
          "duplicated tag identifier \"%s\"",
          compiler->last_error_extra_info);
      break;
    case ERROR_DUPLICATED_META_IDENTIFIER:
      snprintf(
          buffer,
          buffer_size,
          "duplicated metadata identifier \"%s\"",
          compiler->last_error_extra_info);
      break;
    case ERROR_DUPLICATED_LOOP_IDENTIFIER:
      snprintf(
          buffer,
          buffer_size,
          "duplicated loop identifier \"%s\"",
          compiler->last_error_extra_info);
      break;
    case ERROR_UNDEFINED_STRING:
      snprintf(
          buffer,
          buffer_size,
          "undefined string \"%s\"",
          compiler->last_error_extra_info);
      break;
    case ERROR_UNDEFINED_IDENTIFIER:
      snprintf(
          buffer,
          buffer_size,
          "undefined identifier \"%s\"",
          compiler->last_error_extra_info);
      break;
    case ERROR_UNREFERENCED_STRING:
      snprintf(
          buffer,
          buffer_size,
          "unreferenced string \"%s\"",
          compiler->last_error_extra_info);
      break;
    case ERROR_EMPTY_STRING:
      snprintf(
          buffer,
          buffer_size,
          "empty string \"%s\"",
          compiler->last_error_extra_info);
      break;
    case ERROR_NOT_A_STRUCTURE:
      snprintf(
          buffer,
          buffer_size,
          "\"%s\" is not a structure",
          compiler->last_error_extra_info);
      break;
    case ERROR_NOT_INDEXABLE:
      snprintf(
          buffer,
          buffer_size,
          "\"%s\" is not an array or dictionary",
          compiler->last_error_extra_info);
      break;
    case ERROR_NOT_A_FUNCTION:
      snprintf(
          buffer,
          buffer_size,
          "\"%s\" is not a function",
          compiler->last_error_extra_info);
      break;
    case ERROR_INVALID_FIELD_NAME:
      snprintf(
          buffer,
          buffer_size,
          "invalid field name \"%s\"",
          compiler->last_error_extra_info);
      break;
    case ERROR_MISPLACED_ANONYMOUS_STRING:
      snprintf(
          buffer,
          buffer_size,
          "wrong use of anonymous string");
      break;
    case ERROR_INCLUDES_CIRCULAR_REFERENCE:
      snprintf(
          buffer,
          buffer_size,
          "include circular reference");
      break;
    case ERROR_INCLUDE_DEPTH_EXCEEDED:
      snprintf(buffer,
          buffer_size,
          "too many levels of included rules");
      break;
    case ERROR_LOOP_NESTING_LIMIT_EXCEEDED:
      snprintf(buffer,
          buffer_size,
          "loop nesting limit exceeded");
      break;
    case ERROR_NESTED_FOR_OF_LOOP:
      snprintf(buffer,
          buffer_size,
          "'for <quantifier> of <string set>' loops can't be nested");
      break;
    case ERROR_UNKNOWN_MODULE:
      snprintf(
          buffer,
          buffer_size,
          "unknown module \"%s\"",
          compiler->last_error_extra_info);
      break;
    case ERROR_INVALID_MODULE_NAME:
      snprintf(
          buffer,
          buffer_size,
          "invalid module name \"%s\"",
          compiler->last_error_extra_info);
      break;
    case ERROR_DUPLICATED_STRUCTURE_MEMBER:
      snprintf(buffer,
          buffer_size,
          "duplicated structure member");
      break;
    case ERROR_WRONG_ARGUMENTS:
      snprintf(
          buffer,
          buffer_size,
          "wrong arguments for function \"%s\"",
          compiler->last_error_extra_info);
      break;
    case ERROR_WRONG_RETURN_TYPE:
      snprintf(buffer,
          buffer_size,
          "wrong return type for overloaded function");
      break;
    case ERROR_INVALID_HEX_STRING:
    case ERROR_INVALID_REGULAR_EXPRESSION:
    case ERROR_SYNTAX_ERROR:
    case ERROR_WRONG_TYPE:
      snprintf(
          buffer,
          buffer_size,
          "%s",
          compiler->last_error_extra_info);
      break;
    case ERROR_INTERNAL_FATAL_ERROR:
      snprintf(
          buffer,
          buffer_size,
          "internal fatal error");
      break;
    case ERROR_DIVISION_BY_ZERO:
      snprintf(
          buffer,
          buffer_size,
          "division by zero");
      break;
    case ERROR_REGULAR_EXPRESSION_TOO_LARGE:
      snprintf(
          buffer,
          buffer_size,
          "regular expression is too large");
      break;
    case ERROR_REGULAR_EXPRESSION_TOO_COMPLEX:
      snprintf(
          buffer,
          buffer_size,
          "regular expression is too complex");
      break;
    case ERROR_TOO_MANY_STRINGS:
       yr_get_configuration(
          YR_CONFIG_MAX_STRINGS_PER_RULE,
          &max_strings_per_rule);
       snprintf(
          buffer,
          buffer_size,
          "too many strings in rule \"%s\" (limit: %d)",
          compiler->last_error_extra_info,
          max_strings_per_rule);
      break;
  }

  return buffer;
}
