#include <stdio.h>
#include "gm_backend_gps.h"
#include "gm_error.h"
#include "gm_code_writer.h"
#include "gm_frontend.h"
#include "gm_transform_helper.h"
#include "gm_builtin.h"

//------------------------------------------------------------------------------------
// Check things related to the edge property
//
// [1] Conditions 
//   - Access to edge property is available only through edge variable that is defined
//     inside the inner-loop.
//   - Edge variable, defined inside a inner-loop, should be initialized as (2nd-level iterator).toEdge.
//   - 2nd level iteration should use out-going edges only.
//          
//     Foreach(n: G.Nodes)  {
//        Foreach(s: n.Nbrs) {
//            Edge(G) e = s.ToEdge();
//     }  }
//
// [2] Writing to Edge property 
//   - Write to edge-property should be 'simple' (i.e. not conditional)
//   - RHSs of edge-prop writing are not mapped into communication
//   - RHSs of edge prop writing cannot condtain inner-loop scoped symbol
//       Foreach(n: G.Nodes)  {
//          Foreach(s: n.Nbrs)  {
//            Edge(G) e = s.ToEdge();
//            if (s.Y > 10) e.C = 10; // error
//            e.A = e.B + n.Y;  // okay. n.Y or e.B is not transported, but locally used.
//            e.B = s.Y; // error
//       }  }
//
// [3] Reading from Edge property
//    - Possible Edge-Prop Access Sequence
//        - Sent : okay
//        - Write : okay
//        - Write -> sent : okay 
//        - Send -> Write : okay
//        - Write -> Send -> Write: okay
//        - Send -> Write -> Send: Error
//           ==> because message cannot hold two versions of edge property 
//
//      Foreach(n: G.Nodes) {
//         Foreach(s: n.Nbrs) {
//            Edge(g) e = s.toEdge();
//            e.A = n.Y;                // okay.  A: write
//            s.Z += e.A + e.B;         // okay   A: write-sent, B :sent
//            e.B = n.Y+1;              // okay.  B: sent-write
//            e.A = 0;                  // okay.  A: (write-)sent-write
//            s.Z += e.B;               // Error, B: (Send-write-Send)
//         }
//      }
//------------------------------------------------------------------------------------
// Implementation
//    - Inner loop maintains a map of edge-prop symbols
//         <symbol, state>
//    - Inner loop maintains a list of edge-prop writes
//
//
// Additional Information creted
//     GPS_MAP_EDGE_PROP_ACCESS :   <where:>foreach, <what:> map(symbol,int(state)), one of GPS_ENUM_EDGE_VALUE_xxx
//     GPS_FLAG_EDGE_DEFINED_INNER: <where:>var symbol(edge type),<what:>if the varaible is defined inside inner loop
//     GPS_FLAG_EDGE_DEFINING_INNTER: <where:>foreach, <what:>if this inner loop defines an edge variable
//     GPS_LIST_EDGE_PROP_WRITE: <where:>foreach, <what:> (list of) assigns whose target is edge variables
//     GPS_FLAG_EDGE_DEFINING_WRITING:<where:>assign, <what:>if this assignment is defining en edge (as inner.ToEdge())
//------------------------------------------------------------------------------------

static const int SENDING = 1;
static const int WRITING = 2;

// return: is_error
static bool manage_edge_prop_access_state(ast_foreach* fe, gm_symtab_entry* e, int op) {
    assert((op==SENDING) || (op==WRITING));
    int* curr_state = (int*) fe->find_info_map_value(GPS_MAP_EDGE_PROP_ACCESS, e);

    // first access
    if (curr_state == NULL) {
        int* new_state = new int;
        *new_state = (op == SENDING) ? GPS_ENUM_EDGE_VALUE_SENT : GPS_ENUM_EDGE_VALUE_WRITE;

        fe->add_info_map_key_value(GPS_MAP_EDGE_PROP_ACCESS, e, new_state);
    } else {
        int curr_state_val = *curr_state;
        switch (curr_state_val) {
            case GPS_ENUM_EDGE_VALUE_ERROR: //already error
                return false;

            case GPS_ENUM_EDGE_VALUE_WRITE:
                if (op == SENDING) *curr_state = GPS_ENUM_EDGE_VALUE_WRITE_SENT;
                return false; // no error

            case GPS_ENUM_EDGE_VALUE_SENT:
                if (op == WRITING) *curr_state = GPS_ENUM_EDGE_VALUE_SENT_WRITE;
                return false; // no error

            case GPS_ENUM_EDGE_VALUE_WRITE_SENT:
                if (op == WRITING) *curr_state = GPS_ENUM_EDGE_VALUE_SENT_WRITE;
                return false;

            case GPS_ENUM_EDGE_VALUE_SENT_WRITE:
                if (op == SENDING) { // sending two versions!
                    *curr_state = GPS_ENUM_EDGE_VALUE_ERROR;
                    return true; // ERROR
                } else
                    return false;
            default:
                assert(false);
                break;
        }
    }
    return false;
}

class gps_check_edge_value_t : public gm_apply
{
public:
    gps_check_edge_value_t() {
        set_separate_post_apply(true);
        set_for_symtab(true);
        set_for_sent(true);
        set_for_expr(true);
        inner_iter = NULL;
        inner_loop = NULL;
        _error = false;
        target_is_edge_prop = false;
    }

    bool is_error() {
        return _error;
    }
    void set_error(bool b) {
        _error = b;
    }

    virtual bool apply(gm_symtab_entry *e, int type) {
        if (e->getType()->is_edge() && (inner_loop != NULL)) {
            e->add_info_bool(GPS_FLAG_EDGE_DEFINED_INNER, true);
            inner_loop->add_info_bool(GPS_FLAG_EDGE_DEFINING_INNER, true);
        }
        return true;
    }

    virtual bool apply(ast_sent* s) {
        if (s->get_nodetype() == AST_FOREACH) {
            ast_foreach* fe = (ast_foreach*) s;
            if (fe->find_info_bool(GPS_FLAG_IS_INNER_LOOP)) {
                inner_iter = fe->get_iterator()->getSymInfo();
                inner_loop = fe;
            }
        } else if (s->get_nodetype() == AST_ASSIGN) {
            ast_assign * a = (ast_assign*) s;
            if (!a->is_target_scalar()) {
                gm_symtab_entry* sym = a->get_lhs_field()->get_first()->getSymInfo();
                if (sym->getType()->is_edge_compatible()) {

                    if (sym->find_info_bool(GPS_FLAG_EDGE_DEFINED_INNER)) {
                        ast_sent* parent = (ast_sent*) s->get_parent();

                        // check if conditional write
                        bool conditional = false;
                        while (true) {
                            if (parent == inner_loop) break;

                            if ((parent->get_nodetype() == AST_WHILE) || (parent->get_nodetype() == AST_IF) || (parent->get_nodetype() == AST_FOREACH)) {
                                conditional = true;
                                break;
                            }
                            parent = (ast_sent*) parent->get_parent();
                            assert(parent!=NULL);
                        }

                        if (conditional) {
                            gm_backend_error(GM_ERROR_GPS_EDGE_WRITE_CONDITIONAL, a->get_lhs_field()->get_line(), a->get_lhs_field()->get_col());
                            set_error(true);
                        }

                        target_is_edge_prop = true;

                        // add write symbol
                        assert(inner_loop!=NULL);
                        inner_loop->add_info_list_element(GPS_LIST_EDGE_PROP_WRITE, s);

                        gm_symtab_entry* target = a->get_lhs_field()->get_second()->getSymInfo();
                        bool b = manage_edge_prop_access_state(inner_loop, target, WRITING);
                        assert(b == false);

                        // [TODO]
                        // grouped assignment?

                    } else {
                        /*
                         gm_backend_error(GM_ERROR_GPS_EDGE_WRITE_RANDOM,
                         a->get_lhs_field()->get_line(),
                         a->get_lhs_field()->get_col());
                         set_error(true);
                         */
                    }
                }
            } else { // lhs scala
                gm_symtab_entry* sym = a->get_lhs_scala()->getSymInfo();
                if (sym->getType()->is_edge()) {
                    if (sym->find_info_bool(GPS_FLAG_EDGE_DEFINED_INNER)) {
                        // check rhs is to - edge
                        ast_expr* rhs = a->get_rhs();
                        if (rhs->is_builtin()) {
                            ast_expr_builtin* b_rhs = (ast_expr_builtin*) rhs;
                            gm_symtab_entry *drv = b_rhs->get_driver()->getSymInfo();
                            int f_id = b_rhs->get_builtin_def()->get_method_id();
                            if (f_id == GM_BLTIN_NODE_TO_EDGE) {
                                a->add_info_bool(GPS_FLAG_EDGE_DEFINING_WRITE, true);
                            }
                        }

                        /*
                         if (error) {
                         set_error(error);
                         gm_backend_error(GM_ERROR_GPS_EDGE_WRITE_RANDOM,
                         a->get_lhs_scala()->get_line(),
                         a->get_lhs_scala()->get_col());
                         }
                         */

                    }
                }
            }
        }

        return true;
    }

    // random edge read is not allowed.
    virtual bool apply(ast_expr* e) {
        //-----------------------------------------------
        // Edge f = ...
        // Foreach (t: G.Nodes) {
        //    Foreach (n: t.Nbrs) { 
        //       Edge e = n.ToEdge();
        //       Int x = f.A;    // (case 1) random reading 
        //       e.A = n.X;      // (case 2) inner scoped rhs
        //
        //       ... = e.A;
        //       e.A = ...;      // (case 3) sending two versions
        //       ... = e.A;
        //      
        //    }
        // }
        //-----------------------------------------------

        // checking of (case 2)
        if (target_is_edge_prop) {
            if ((e->find_info_bool(GPS_INT_EXPR_SCOPE) == GPS_NEW_SCOPE_IN) || (e->find_info_bool(GPS_INT_EXPR_SCOPE) == GPS_NEW_SCOPE_RANDOM)) {
                if (e->is_field()) {
                    ast_field* f = e->get_field();
                    gm_backend_error(GM_ERROR_GPS_EDGE_WRITE_RHS, f->get_line(), f->get_col(), f->get_first()->get_orgname());
                    set_error(true);
                } else if (e->is_id()) {
                    ast_id* f = e->get_id();
                    gm_backend_error(GM_ERROR_GPS_EDGE_WRITE_RHS, f->get_line(), f->get_col(), f->get_orgname());
                    set_error(true);
                }
            }
        }

        if (e->is_field()) {
            ast_field* f = e->get_field();
            if (f->getSourceTypeInfo()->is_edge_compatible()) {
                // check if random reading (case 1)
                if (!f->get_first()->getSymInfo()->find_info_bool(GPS_FLAG_EDGE_DEFINED_INNER)) {
                    gm_backend_error(GM_ERROR_GPS_EDGE_READ_RANDOM, f->get_line(), f->get_col());
                    set_error(true);
                } else {
                    // (case 3)
                    bool b = manage_edge_prop_access_state(inner_loop, f->get_second()->getSymInfo(), SENDING);

                    if (b) {
                        gm_backend_error(GM_ERROR_GPS_EDGE_SEND_VERSIONS, f->get_line(), f->get_col(), f->get_first()->get_orgname());
                        set_error(true);
                    }
                }
            }
        }

        return true;
    }

    virtual bool apply2(ast_sent* s) {
        if (s->get_nodetype() == AST_FOREACH) {
            if (((ast_foreach*) s) == inner_loop) {
                inner_loop = NULL;
                inner_iter = NULL;
            }
        } else if (s->get_nodetype() == AST_ASSIGN) {
            target_is_edge_prop = false;
        }
        return true;
    }
private:
    gm_symtab_entry* inner_iter;
    ast_foreach* inner_loop;
    bool target_is_edge_prop;
    bool _error;
};

void gm_gps_opt_check_edge_value::process(ast_procdef* proc) {
    gps_check_edge_value_t T2;
    proc->traverse_both(&T2);
    set_okay(!T2.is_error());
    return;
}
