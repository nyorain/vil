#ifndef __SPIRV_VM_STATE_H__
#define __SPIRV_VM_STATE_H__

#include <spvm/program.h>
#include <spvm/result.h>
#include <spvm/analyzer.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

struct spvm_state;
struct spvm_image;

typedef struct spvm_stack_entry {
	spvm_source source;
	spvm_result_t function;
	spvm_word function_stack_returns;
	spvm_word function_stack_cfg;
	const char* file;
	spvm_word line;
} spvm_stack_entry;

typedef struct spvm_member_list {
	unsigned member_count;
	spvm_member* members;
} spvm_member_list;

// When loading: should evaluate the OpVariable varID with the given access chain
//   indiices and copy it into the given members list. The members are already
//   initialized to the correct types, just their values need to be set.
// When storing: should take the given members and store them into the
//   memory pointed to by the given varID and access chain indices.
typedef void(*spvm_access_variable_fn)(struct spvm_state*, unsigned varID,
	unsigned index_count, const spvm_word* indices,
	spvm_member_list members, spvm_word res_type_id);
typedef unsigned(*spvm_array_length_fn)(struct spvm_state*, unsigned varID,
	unsigned index_count, const spvm_word* indices);

typedef spvm_vec4f (*spvm_image_read_fn)(struct spvm_state*, struct spvm_image*,
	int x, int y, int z, int layer, int level);
typedef void (*spvm_image_write_fn)(struct spvm_state*, struct spvm_image*,
	int x, int y, int z, int layer, int level, const spvm_vec4f* data);

typedef void(*spvm_log_fn)(struct spvm_state*, const char* fmt, va_list);

typedef struct spvm_state {
	spvm_context_t context;
	spvm_program_t owner;
	spvm_source code_current; // current position in the code
	spvm_result* results;

	spvm_byte did_jump;
	spvm_byte discarded;

	spvm_result_t current_function;

	spvm_word current_parameter;

	spvm_word function_stack_current;
	spvm_word function_stack_count;
	spvm_source* function_stack;
	spvm_result_t* function_stack_info;
	spvm_word return_id;
	spvm_word* function_stack_returns;
	spvm_word* function_stack_cfg;
	spvm_word* function_stack_cfg_parent;

	void(*emit_vertex)(struct spvm_state*, spvm_word);
	void(*end_primitive)(struct spvm_state*, spvm_word);

	void(*control_barrier)(struct spvm_state*, spvm_word, spvm_word, spvm_word);

	// Will be called every time the shader accesses a variable.
	// Will only be called while executing code, not during setup.
	// TODO: maybe just pass id of AccessChain in case of access chain store/load?
	// Would allow removing the indices from the call, letting the caller
	// figure it out. And would probably allow the caller to use the
	// members of the result as cache.
	spvm_access_variable_fn load_variable;
	spvm_access_variable_fn store_variable;
	spvm_array_length_fn array_length;

	spvm_image_read_fn read_image;
	spvm_image_write_fn write_image;
	spvm_log_fn log;

	float frag_coord[4];

	// derivative group
	spvm_byte _derivative_is_group_member;
	spvm_byte derivative_used;
	float derivative_buffer_x[16];
	float derivative_buffer_y[16];
	struct spvm_state* derivative_group_x; // right
	struct spvm_state* derivative_group_y; // bottom
	struct spvm_state* derivative_group_d; // bottom right / diagonal

	// debug information
	const char* current_file;
	spvm_word current_line;
	spvm_word current_column;
	spvm_word instruction_count;

	// pointer to analyzer
	spvm_analyzer_t analyzer;

	void* user_data;
} spvm_state;
typedef spvm_state* spvm_state_t;

typedef struct spvm_state_settings {
	spvm_access_variable_fn load_variable;
	spvm_access_variable_fn store_variable;
	spvm_log_fn log;

	spvm_byte force_derv;
	spvm_byte is_derv_member;
} spvm_state_settings;

spvm_result_t spvm_state_get_type_info(spvm_result_t res_list, spvm_result_t res);

spvm_state_t spvm_state_create(spvm_program_t prog, spvm_state_settings);
void spvm_state_set_extension(spvm_state_t state, const char* name, spvm_ext_opcode_func* ext);
void spvm_state_call_function(spvm_state_t state);
void spvm_state_prepare(spvm_state_t state, spvm_word fnLocation);
void spvm_state_copy_uniforms(spvm_state_t dst, spvm_state_t src);
void spvm_state_set_frag_coord(spvm_state_t state, float x, float y, float z, float w);
void spvm_state_ddx(spvm_state_t state, spvm_word id);
void spvm_state_ddy(spvm_state_t state, spvm_word id);
void spvm_state_group_sync(spvm_state_t state);
void spvm_state_group_step(spvm_state_t state);
void spvm_state_step_opcode(spvm_state_t state);
void spvm_state_step_into(spvm_state_t state);
void spvm_state_jump_to(spvm_state_t state, spvm_word line);
void spvm_state_jump_to_instruction(spvm_state_t state, spvm_word instruction_count);
spvm_word spvm_state_get_result_location(spvm_state_t state, const char* str);
spvm_member_t spvm_state_get_builtin(spvm_state_t state, SpvBuiltIn decor, spvm_word* mem_count);
spvm_result_t spvm_state_get_result(spvm_state_t state, const char* str);
spvm_result_t spvm_state_get_result_with_value(spvm_state_t state, const char* str);
spvm_result_t spvm_state_get_local_result(spvm_state_t state, spvm_result_t fn, const char* str);
spvm_member_t spvm_state_get_object_member(spvm_state_t state, spvm_result_t var, const char* member_name);
void spvm_state_push_function_stack(spvm_state_t state, spvm_result_t func, spvm_word func_res_id);
void spvm_state_pop_function_stack(spvm_state_t state);
void spvm_state_delete(spvm_state_t state);
void spvm_state_log(struct spvm_state* state, const char* fmt, ...);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // __SPIRV_VM_STATE_H__
