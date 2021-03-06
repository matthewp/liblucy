#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "node.h"
#include "program.h"
#include "parser.h"
#include "str_builder.h"
#include "js_builder.h"
#include "compiler_xstate.h"

// API flags
#define FLAG_USE_REMOTE 1 << 0

// Machine implementation flags
#define XS_HAS_STATE_PROP 1 << 0

typedef struct Ref {
  char* key;
  struct Ref* next;
  Expression* value;
} Ref;

typedef struct PrintState {
  bool on_prop_added;
  bool always_prop_added;
  Ref* guard;
  Ref* action;
} PrintState;

static void add_action_ref(PrintState* state, char* key, Expression* value) {
  Ref *ref = malloc(sizeof(Ref));
  ref->key = key;
  ref->value = node_clone_expression(value);
  ref->next = NULL;

  if(state->action == NULL) {
    state->action = ref;
  } else {
    Ref* cur = state->action;
    while(cur->next != NULL) {
      cur = cur->next;
    }
    cur->next = ref;
  }
}

static void add_guard_ref(PrintState* state, char* key, Expression* value) {
  Ref *ref = malloc(sizeof(Ref));
  ref->key = key;
  ref->value = node_clone_expression(value);
  ref->next = NULL;

  if(state->guard == NULL) {
    state->guard = ref;
  } else {
    Ref* cur = state->guard;
    while(cur->next != NULL) {
      cur = cur->next;
    }
    cur->next = ref;
  }
}

static void destroy_ref(Ref* ref) {
  if(ref->next != NULL) {
    destroy_ref(ref->next);
  }
  if(ref->value != NULL) {
    node_destroy_expression(ref->value);
  }
  free(ref);
}

static void destroy_state(PrintState *state) {
  if(state->guard != NULL) {
    destroy_ref(state->guard);
  }
  if(state->action != NULL) {
    destroy_ref(state->action);
  }
}

static void enter_machine(PrintState* state, JSBuilder* jsb, Node* node) {
  MachineNode *machine_node = (MachineNode*)node;
  Node* parent_node = node->parent;
  bool is_nested = node_machine_is_nested(node);

  if(!is_nested) {
    if(machine_node->name == NULL) {
      js_builder_add_str(jsb, "\nexport default ");
    } else {
      js_builder_add_export(jsb);
      js_builder_add_const(jsb, machine_node->name);
      js_builder_add_str(jsb, " = ");
    }
    js_builder_start_call(jsb, "Machine");
    js_builder_start_object(jsb);
  } else {
    // Close out the on prop
    if(state->on_prop_added) {
      state->on_prop_added = false;
      js_builder_end_object(jsb);
    }
  }

  if(machine_node->initial != NULL) {
    js_builder_start_prop(jsb, "initial");
    js_builder_add_string(jsb, machine_node->initial);
  }
}

static void exit_machine(PrintState* state, JSBuilder* jsb, Node* node) {
  bool has_guard = state->guard != NULL;
  bool has_action = state->action != NULL;
  bool needs_options = has_guard || has_action;
  bool is_nested = node_machine_is_nested(node);

  if(!is_nested && needs_options) {
    js_builder_end_object(jsb);
    js_builder_add_str(jsb, ", ");

    js_builder_start_object(jsb);

    if(has_guard) {
      js_builder_start_prop(jsb, "guards");
      js_builder_start_object(jsb);

      Ref* ref = state->guard;
      while(ref != NULL) {
        js_builder_start_prop(jsb, ref->key);

        Expression* expression = ref->value;

        if(expression->type != EXPRESSION_IDENTIFIER) {
          printf("Unexpected type of expression\n");
          break;
        }

        char* identifier = ((IdentifierExpression*)expression)->name;
        js_builder_add_str(jsb, identifier);

        ref = ref->next;
      }

      js_builder_end_object(jsb);
    }

    if(has_action) {
      js_builder_start_prop(jsb, "actions");
      js_builder_start_object(jsb);

      Ref* ref = state->action;
      while(ref != NULL) {
        js_builder_start_prop(jsb, ref->key);

        Expression* expression = ref->value;

        switch(expression->type) {
          case EXPRESSION_ASSIGN: {
            AssignExpression* assign = (AssignExpression*)expression;

            js_builder_start_call(jsb, "assign");
            js_builder_start_object(jsb);
            js_builder_start_prop(jsb, assign->key);
            js_builder_add_str(jsb, assign->identifier);
            js_builder_end_object(jsb);
            js_builder_end_call(jsb);

            //js_builder_add_string(jsb, assign->identifier);
            break;
          }
          default: {
            printf("This type of expression is not currently supported.\n");
            break;
          }
        }

        ref = ref->next;
      }

      js_builder_end_object(jsb);
    }

    js_builder_end_object(jsb);
  } else if(!is_nested) {
    js_builder_end_object(jsb);
  }

  if(!is_nested) {
    js_builder_end_call(jsb);
    js_builder_add_str(jsb, ";");
  }
}

static void enter_import(PrintState* state, JSBuilder* jsb, Node* node) {
  js_builder_add_str(jsb, "import ");

  ImportNode *import_node = (ImportNode*)node;
  Node* child = node->child;
  bool multiple = false;

  if(child == NULL) {
    printf("TODO add support for imports with no specifiers\n");
    return;
  }

  js_builder_add_str(jsb, "{ ");

  while(child != NULL) {
    ImportSpecifier *specifier = (ImportSpecifier*)child;

    if(multiple) {
      js_builder_add_str(jsb, ", ");
    }

    js_builder_add_str(jsb, specifier->imported);
    // TODO support local

    child = child->next;
    multiple = true;
  }

  js_builder_add_str(jsb, " } from ");
  js_builder_add_str(jsb, import_node->from);
  js_builder_add_str(jsb, ";\n");
}

static void enter_state(PrintState* state, JSBuilder* jsb, Node* node) {
  StateNode* state_node = (StateNode*)node;
  MachineNode* machine_node = (MachineNode*)node->parent;
  if(!(machine_node->impl_flags & XS_HAS_STATE_PROP)) {
    machine_node->impl_flags |= XS_HAS_STATE_PROP;
    js_builder_start_prop(jsb, "states");
    js_builder_start_object(jsb);
  }

  js_builder_start_prop(jsb, state_node->name);
  js_builder_start_object(jsb);

  if(state_node->final) {
    js_builder_start_prop(jsb, "type");
    js_builder_add_string(jsb, "final");
  }
}

static void exit_state(PrintState* state, JSBuilder* jsb, Node* node) {
  js_builder_end_object(jsb);

  StateNode* state_node = (StateNode*)node;

  // End of all state nodes
  if(!node->next || node->next->type != NODE_STATE_TYPE) {
    js_builder_end_object(jsb);
  }
}

static void enter_invoke(PrintState* state, JSBuilder* jsb, Node* node) {
  js_builder_start_prop(jsb, "invoke");
  js_builder_start_object(jsb);

  InvokeNode* invoke_node = (InvokeNode*)node;
  js_builder_start_prop(jsb, "src");
  js_builder_add_str(jsb, invoke_node->call);
}

static void exit_invoke(PrintState* state, JSBuilder* jsb, Node* node) {
  js_builder_end_object(jsb);
}

static void enter_transition(PrintState* state, JSBuilder* jsb, Node* node) {
  TransitionNode* transition_node = (TransitionNode*)node;
  Node* parent_node = node->parent;
  char* event_name = transition_node->event;
  int type = transition_node->type;

  bool is_always = transition_node->type == TRANSITION_IMMEDIATE_TYPE;

  if(parent_node->type == NODE_INVOKE_TYPE) {
    if(strcmp(event_name, "done") == 0) {
      js_builder_start_prop(jsb, "onDone");
    } else if(strcmp(event_name, "error") == 0) {
      js_builder_start_prop(jsb, "onError");
    } else {
      printf("Regular events in invoke are not supported.\n");
    }
  } else {
    switch(type) {
      case TRANSITION_EVENT_TYPE: {
        if(!state->on_prop_added) {
          state->on_prop_added = true;
          js_builder_start_prop(jsb, "on");
          js_builder_start_object(jsb);
        }

        js_builder_start_prop(jsb, event_name);
        break;
      }
      case TRANSITION_IMMEDIATE_TYPE: {
        if(!state->always_prop_added) {
          state->always_prop_added = true;
          js_builder_start_prop(jsb, "always");
          js_builder_start_array(jsb, true);
          js_builder_add_indent(jsb);
        } else {
          js_builder_add_str(jsb, ", ");
        }
        break;
      }
      case TRANSITION_DELAY_TYPE: {
        js_builder_start_prop(jsb, "delay");
        js_builder_start_object(jsb);

        int ms = transition_node->delay->ms;
        int length = (int)((ceil(log10(ms))+1)*sizeof(char));
        char str[length];
        sprintf(str, "%i", ms);
        js_builder_start_prop(jsb, str);
        break;
      }
    }
  }

  bool has_guard = transition_node->guard != NULL;
  bool has_action = transition_node->action != NULL;
  bool has_guard_or_action = has_guard || has_action;
  bool use_object_notation = has_guard_or_action || is_always;

  if(use_object_notation) {
    js_builder_start_object(jsb);
    js_builder_start_prop(jsb, "target");
    js_builder_add_string(jsb, transition_node->dest);

    if(has_guard) {
      js_builder_start_prop(jsb, "cond");

      TransitionGuard* guard = transition_node->guard;
      // If there are multiple guards use an array.
      if(guard->next) {
        js_builder_start_array(jsb, false);
        while(true) {
          if(guard->name != NULL) {
            js_builder_add_string(jsb, guard->name);
          } else {
            // Expression!
            js_builder_add_str(jsb, guard->expression->ref);
          }

          
          guard = guard->next;

          if(guard == NULL) {
            break;
          }

          js_builder_add_str(jsb, ", ");
        }

        js_builder_end_array(jsb, false);
      }
      // If a single guard use a string
      else {
        if(guard->name != NULL) {
          js_builder_add_string(jsb, guard->name);
        } else {
          // Expression!
          js_builder_add_str(jsb, guard->expression->ref);
        }
      }
    }

    if(has_action) {
      TransitionAction* action = transition_node->action;
      TransitionAction* inner = action;
      bool use_multiline = false;
      while(inner && !use_multiline) {
        if(action->expression && action->expression->type == EXPRESSION_ASSIGN) {
          use_multiline = true;
          break;
        }
        inner = inner->next;
      }
      js_builder_start_prop(jsb, "actions");
      js_builder_start_array(jsb, use_multiline);

      while(true) {
        if(action->name != NULL) {
          js_builder_add_string(jsb, action->name);
        } else {
          // Inline assign!
          unsigned short expression_type = action->expression->type;
          switch(expression_type) {
            case EXPRESSION_ASSIGN: {
              AssignExpression* assign_expression = (AssignExpression*)action->expression;
              js_builder_start_call(jsb, "assign");
              js_builder_start_object(jsb);
              js_builder_start_prop(jsb, assign_expression->key);
              js_builder_add_str(jsb, "(context, event) => event.data");
              js_builder_end_object(jsb);
              js_builder_end_call(jsb);
              break;
            }
            case EXPRESSION_ACTION: {
              ActionExpression* action_expression = (ActionExpression*)action->expression;
              if(use_multiline) {
                js_builder_add_indent(jsb);
              }
              js_builder_add_str(jsb, action_expression->ref);
              break;
            }
          }
        }

        action = action->next;

        if(action == NULL) {
          break;
        }

        js_builder_add_str(jsb, ", ");
      }

      js_builder_end_array(jsb, use_multiline);
    }

    js_builder_end_object(jsb);

    if(is_always) {
      if(!node_transition_has_sibling_always(transition_node)) {
        js_builder_end_array(jsb, true);
      }
    }
  } else {
    js_builder_add_string(jsb, transition_node->dest);
  }
}

static void exit_transition(PrintState* state, JSBuilder* jsb, Node* node) {
  if(node->next) {
    
  } else {
    TransitionNode* transition_node = (TransitionNode*)node;
    if(state->on_prop_added) {
      state->on_prop_added = false;
      js_builder_end_object(jsb);
    }

    if(state->always_prop_added) {
      state->always_prop_added = false;
    }

    if(transition_node->type == TRANSITION_DELAY_TYPE) {
      js_builder_end_object(jsb);
    }
  }
}

static void enter_assignment(PrintState* state, JSBuilder* jsb, Node* node) {
  Assignment* assignment = (Assignment*)node;

  switch(assignment->binding_type) {
    case ASSIGNMENT_ACTION: {
      add_action_ref(state, assignment->binding_name, assignment->value);
      break;
    }
    case ASSIGNMENT_GUARD: {
      add_guard_ref(state, assignment->binding_name, assignment->value);
      break;
    }
  }
}

CompileResult* xs_create() {
  CompileResult* result = malloc(sizeof(*result));
  return result;
}

void xs_init(CompileResult* result, int use_remote_source) {
  result->success = false;
  result->js = NULL;
  result->flags = 0;
  
  if(use_remote_source) {
    result->flags |= FLAG_USE_REMOTE;
  }
}

void compile_xstate(CompileResult* result, char* source, char* filename) {
  ParseResult *parse_result = parse(source, filename);

  if(parse_result->success == false) {
    result->success = false;
    result->js = NULL;
    return;
  }

  Program *program = parse_result->program;
  char* xstate_specifier;
  if(result->flags & FLAG_USE_REMOTE) {
    xstate_specifier = "https://cdn.skypack.dev/xstate";
  } else {
    xstate_specifier = "xstate";
  }

  JSBuilder *jsb;
  Node* node;
  jsb = js_builder_create();
  node = program->body;

  if(node != NULL) {
    js_builder_add_str(jsb, "import { Machine");

    if(program->flags & PROGRAM_USES_ASSIGN) {
      js_builder_add_str(jsb, ", assign");
    }

    js_builder_add_str(jsb, " } from '");

    js_builder_add_str(jsb, xstate_specifier);
    js_builder_add_str(jsb, "';\n");
  }

  PrintState state = {
    .on_prop_added = false,
    .always_prop_added = false,
    .guard = NULL,
    .action = NULL
  };

  bool exit = false;
  while(node != NULL) {
    unsigned short type = node->type;

    if(exit) {
      switch(type) {
        case NODE_STATE_TYPE: {
          exit_state(&state, jsb, node);
          node_destroy_state((StateNode*)node);
          break;
        }
        case NODE_TRANSITION_TYPE: {
          exit_transition(&state, jsb, node);
          node_destroy_transition((TransitionNode*)node);
          break;
        }
        case NODE_ASSIGNMENT_TYPE: {
          node_destroy_assignment((Assignment*)node);
          break;
        }
        case NODE_IMPORT_SPECIFIER_TYPE: {
          node_destroy_import_specifier((ImportSpecifier*)node);
          break;
        }
        case NODE_IMPORT_TYPE: {
          node_destroy_import((ImportNode*)node);
          break;
        }
        case NODE_MACHINE_TYPE: {
          exit_machine(&state, jsb, node);
          node_destroy_machine((MachineNode*)node);
          break;
        }
        case NODE_INVOKE_TYPE: {
          exit_invoke(&state, jsb, node);
          node_destroy_invoke((InvokeNode*)node);
          break;
        }
        default: {
          printf("Node type %hu not torn down (this is a compiler bug)\n", type);
          break;
        }
      }
    } else {
      switch(type) {
        case NODE_IMPORT_TYPE: {
          enter_import(&state, jsb, node);
          break;
        }
        case NODE_STATE_TYPE: {
          enter_state(&state, jsb, node);
          break;
        }
        case NODE_TRANSITION_TYPE: {
          enter_transition(&state, jsb, node);
          break;
        }
        case NODE_ASSIGNMENT_TYPE: {
          enter_assignment(&state, jsb, node);
          break;
        }
        case NODE_INVOKE_TYPE: {
          enter_invoke(&state, jsb, node);
          break;
        }
        case NODE_MACHINE_TYPE: {
          enter_machine(&state, jsb, node);
          break;
        }
      }
    }

    // Node has no children, so go again for the exit.
    if(!exit && !node->child) {
      exit = true;
      continue;
    }
    // Node has a child
    else if(!exit && node->child) {
      node = node->child;
    } else if(node->next) {
      exit = false;
      node = node->next;
    } else if(node->parent) {
      exit = true;
      node_destroy(node);
      node = node->parent;
    } else {
      // Reached the end.
      node_destroy(node);
      break;
    }
  }

  char* js = js_builder_dump(jsb);
  result->success = true;
  result->js = js;

  // Teardown
  destroy_state(&state);
  js_builder_destroy(jsb);

  return;
}

char* xs_get_js(CompileResult* result) {
  return result->js;
}

void destroy_xstate_result(CompileResult* result) {
  if(result->js != NULL) {
    free(result->js);
  }
  free(result);
}