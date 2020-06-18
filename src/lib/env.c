#include "rlang.h"
#include <Rversion.h>

sexp* eval_with_x(sexp* call, sexp* x);
sexp* eval_with_xy(sexp* call, sexp* x, sexp* y);
sexp* eval_with_xyz(sexp* call, sexp* x, sexp* y, sexp* z);


sexp* r_ns_env(const char* pkg) {
  sexp* ns = r_env_find(R_NamespaceRegistry, r_sym(pkg));
  if (ns == r_unbound_sym) {
    r_abort("Can't find namespace `%s`", pkg);
  }
  return ns;
}

static sexp* ns_env_get(sexp* env, const char* name) {
  sexp* obj = KEEP(r_env_find(env, r_sym(name)));

  // Can be a promise to a lazyLoadDBfetch() call
  if (r_typeof(obj) == PROMSXP) {
    obj = r_eval(obj, r_empty_env);
  }
  if (obj != r_unbound_sym) {
    FREE(1);
    return obj;
  }

  // Trigger object not found error
  r_eval(r_sym(name), env);
  r_abort("Internal error: `ns_env_get()` should have failed earlier");
}
sexp* r_base_ns_get(const char* name) {
  return ns_env_get(r_base_env, name);
}


static sexp* rlang_ns_env = NULL;

sexp* rlang_ns_get(const char* name) {
  return ns_env_get(rlang_ns_env, name);
}


static sexp* new_env_call = NULL;
static sexp* new_env__parent_node = NULL;
static sexp* new_env__size_node = NULL;

sexp* r_new_environment(sexp* parent, r_ssize size) {
  parent = parent ? parent : r_empty_env;
  r_node_poke_car(new_env__parent_node, parent);

  size = size ? size : 29;
  r_node_poke_car(new_env__size_node, r_int(size));

  sexp* env = r_eval(new_env_call, r_base_env);

  // Free for gc
  r_node_poke_car(new_env__parent_node, r_null);

  return env;
}


static sexp* env2list_call = NULL;
static sexp* list2env_call = NULL;

sexp* r_env_as_list_compat(sexp* env, sexp* out);

sexp* r_env_as_list(sexp* env) {
  sexp* out = KEEP(eval_with_x(env2list_call, env));

#if R_VERSION < R_Version(4, 0, 0)
  out = r_env_as_list_compat(env, out);
#endif

  FREE(1);
  return out;
}

// On R < 4.0, the active binding function is returned instead of
// its value. We invoke the active bindings here to get consistent
// behaviour in all supported R versions.
sexp* r_env_as_list_compat(sexp* env, sexp* out) {
  sexp* nms = KEEP(r_env_names(env));
  sexp* types = KEEP(r_env_binding_types(env, nms));

  if (types == R_NilValue) {
    FREE(2);
    return out;
  }

  r_ssize n = r_length(nms);
  sexp** nms_ptr = r_chr_deref(nms);
  int* types_ptr = r_int_deref(types);

  for (r_ssize i = 0; i < n; ++i, ++nms_ptr, ++types_ptr) {
    enum r_env_binding_type type = *types_ptr;
    if (type == R_ENV_BINDING_ACTIVE) {
      r_ssize fn_idx = r_chr_detect_index(nms, r_str_deref(*nms_ptr));
      if (fn_idx < 0) {
        r_abort("Internal error: Can't find active binding in list");
      }

      sexp* fn = r_list_get(out, fn_idx);
      sexp* value = r_eval(KEEP(r_call(fn)), r_empty_env);
      r_list_poke(out, fn_idx, value);
      FREE(1);
    }
  }

  FREE(2);
  return out;
}

sexp* r_list_as_environment(sexp* x, sexp* parent) {
  parent = parent ? parent : r_empty_env;
  return eval_with_xy(list2env_call, x, parent);
}

sexp* r_env_clone(sexp* env, sexp* parent) {
  if (parent == NULL) {
    parent = r_env_parent(env);
  }

  sexp* out = KEEP(r_env_as_list(env));
  out = r_list_as_environment(out, parent);

  FREE(1);
  return out;
}


static sexp* remove_call = NULL;

void r_env_unbind_names(sexp* env, sexp* names) {
  eval_with_xyz(remove_call, env, names, r_shared_false);
}
void r_env_unbind_anywhere_names(sexp* env, sexp* names) {
  eval_with_xyz(remove_call, env, names, r_shared_true);
}

sexp* rlang_env_unbind(sexp* env, sexp* names, sexp* inherits) {
  if (r_typeof(env) != r_type_environment) {
    r_abort("`env` must be an environment");
  }
  if (r_typeof(names) != r_type_character) {
    r_abort("`names` must be a character vector");
  }
  if (!r_is_scalar_logical(inherits)) {
    r_abort("`inherits` must be a scalar logical vector");
  }

  if (*r_lgl_deref(inherits)) {
    r_env_unbind_anywhere_names(env, names);
  } else {
    r_env_unbind_names(env, names);
  }

  return r_null;
}

void r_env_unbind_strings(sexp* env, const char** names) {
  sexp* nms = KEEP(r_new_character(names));
  r_env_unbind_names(env, nms);
  FREE(1);
}
void r_env_unbind_anywhere_strings(sexp* env, const char** names) {
  sexp* nms = KEEP(r_new_character(names));
  r_env_unbind_anywhere_names(env, nms);
  FREE(1);
}

void r_env_unbind_string(sexp* env, const char* name) {
  static const char* names[2] = { "", NULL };
  names[0] = name;
  r_env_unbind_strings(env, names);
}
void r_env_unbind_string_anywhere(sexp* env, const char* name) {
  static const char* names[2] = { "", NULL };
  names[0] = name;
  r_env_unbind_anywhere_strings(env, names);
}

bool r_env_inherits(sexp* env, sexp* ancestor, sexp* top) {
  top = top ? top : r_empty_env;

  if (r_typeof(env) != r_type_environment) {
    r_abort("`env` must be an environment");
  }
  if (r_typeof(ancestor) != r_type_environment) {
    r_abort("`ancestor` must be an environment");
  }
  if (r_typeof(top) != r_type_environment) {
    r_abort("`top` must be an environment");
  }

  if (env == r_empty_env) {
    return false;
  }

  while (env != top && env != r_empty_env) {
    if (env == ancestor) {
      return true;
    }
    env = r_env_parent(env);;
  }

  return env == ancestor;
}

void r_init_rlang_ns_env() {
  rlang_ns_env = r_ns_env("rlang");
}

sexp* r_methods_ns_env = NULL;

void r_init_library_env() {
  new_env_call = r_parse_eval("as.call(list(new.env, TRUE, NULL, NULL))", r_base_env);
  r_mark_precious(new_env_call);

  new_env__parent_node = r_node_cddr(new_env_call);
  new_env__size_node = r_node_cdr(new_env__parent_node);

  env2list_call = r_parse("as.list.environment(x, all.names = TRUE)");
  r_mark_precious(env2list_call);

  list2env_call = r_parse("list2env(x, envir = NULL, parent = y, hash = TRUE)");
  r_mark_precious(list2env_call);

  remove_call = r_parse("remove(list = y, envir = x, inherits = z)");
  r_mark_precious(remove_call);

  r_methods_ns_env = r_parse_eval("asNamespace('methods')", r_base_env);
}
