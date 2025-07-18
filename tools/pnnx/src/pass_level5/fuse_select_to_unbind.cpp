// Copyright 2022 Tencent
// SPDX-License-Identifier: BSD-3-Clause

#include "fuse_select_to_unbind.h"

#include <algorithm>
#include "pass_level2.h"

namespace pnnx {

void fuse_select_to_unbind(Graph& graph)
{
    while (1)
    {
        bool matched = false;

        for (size_t i = 0; i < graph.ops.size(); i++)
        {
            Operator* op = graph.ops[i];

            if (op->type != "Tensor.select")
                continue;

            Operand* op_in = op->inputs[0];
            int input_rank = op_in->shape.size();
            if (input_rank == 0)
                continue;

            if (input_rank == 1)
            {
                // skip select scalar
                continue;
            }

            int dim = op->params.at("dim").i;
            const int select_dimsize = op_in->shape[dim];
            if (select_dimsize == -1)
            {
                // skip dynamic
                continue;
            }

            // select 0..n
            std::vector<int> select_n(select_dimsize, 0);
            std::vector<Operator*> select_n_ops(select_dimsize, 0);
            for (auto x : op_in->consumers)
            {
                if (x->type != "Tensor.select")
                    continue;

                if (x->inputs[0] != op_in)
                    continue;

                int dim2 = x->params.at("dim").i;
                int index2 = x->params.at("index").i;

                if (index2 < 0)
                    index2 = select_dimsize + index2;

                if (dim == dim2)
                {
                    select_n[index2] = 1;
                    select_n_ops[index2] = x;
                }
            }

            bool select_full_index = true;
            for (auto x : select_n)
            {
                if (x == 0)
                {
                    select_full_index = false;
                    break;
                }
            }

            if (!select_full_index)
                continue;

            matched = true;

            // delete all select ops and replace with unbind
            Operator* op_unbind = graph.new_operator_before("torch.unbind", op->name, op);
            op_unbind->params["dim"] = dim;

            op_unbind->inputs.push_back(op_in);
            for (int j = 0; j < select_dimsize; j++)
            {
                op_in->consumers.erase(std::find(op_in->consumers.begin(), op_in->consumers.end(), select_n_ops[j]));
            }
            op_in->consumers.push_back(op_unbind);

            op_unbind->outputs.resize(select_dimsize);
            for (int j = 0; j < select_dimsize; j++)
            {
                op_unbind->outputs[j] = select_n_ops[j]->outputs[0];
                select_n_ops[j]->outputs[0]->producer = op_unbind;
            }

            for (int j = 0; j < select_dimsize; j++)
            {
                graph.ops.erase(std::find(graph.ops.begin(), graph.ops.end(), select_n_ops[j]));
                delete select_n_ops[j];
            }

            break;
        }

        if (!matched)
            break;
    }
}

} // namespace pnnx
