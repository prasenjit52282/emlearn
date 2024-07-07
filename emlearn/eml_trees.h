
#ifndef EML_TREES_H
#define EML_TREES_H

#ifndef EML_TREES_TRACE
#define EML_TREES_TRACE 0
#endif

#ifndef EML_TREES_REGRESSION_ENABLE
#define EML_TREES_REGRESSION_ENABLE 1
#endif

#include <stdint.h>
#include <math.h>
#include <eml_common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _EmlTreesNode {
    int8_t feature;
    int16_t value;
    int16_t left;
    int16_t right;
} EmlTreesNode;

/** @typedef EmlTrees
\brief Tree-ensemble

Normally the model initialization is generated by emlearn.

A decision tree is just a special case of an ensemble/forest, with only 1 tree.
*/
typedef struct _EmlTrees {
    int32_t n_nodes;
    EmlTreesNode *nodes;

    int32_t n_trees;
    int32_t *tree_roots;

    int32_t n_leaves;
    uint8_t *leaves;
    int8_t leaf_bits;

    int8_t n_features;
    int8_t n_classes;
} EmlTrees;

typedef enum _EmlTreesError {
    EmlTreesOK = 0,
    EmlTreesUnknownError,
    EmlTreesInvalidClassPredicted,
    EmlTreesErrorLength,
} EmlTreesError;

const char * const eml_trees_errors[EmlTreesErrorLength+1] = {
   "OK",
   "Unknown error",
   "Invalid class predicted",
   "Error length",
};

#ifndef EMTREES_MAX_CLASSES
#define EMTREES_MAX_CLASSES 30
#endif

/*
Make the prediction for an individual decision tree

Returns an offset into the leaves structure
*/
static int32_t
eml_trees_predict_tree(const EmlTrees *forest, int32_t tree_root,
                        const int16_t *features, int8_t features_length)
{
    int32_t node_idx = tree_root;

    // TODO: see if using a pointer node instead of indirect adressing using node_idx improves perf
    while (node_idx >= 0) {
        const int8_t feature = forest->nodes[node_idx].feature;
        const int16_t value = features[feature];
        const int16_t point = forest->nodes[node_idx].value;
        //printf("node %d feature %d. %d < %d\n", node_idx, feature, value, point);
        const int16_t child = (value < point) ? forest->nodes[node_idx].left : forest->nodes[node_idx].right;
    
#if EML_TREES_TRACE
        EML_LOG_PRINTF("predit-tree-iter node=%d feature=%d value=%d th=%d next=%d \n",
            node_idx, feature, value, point, child);
#endif

        if (child >= 0) {
            node_idx += child;
        } else {
            node_idx = child;
        }
    }

    const int16_t leaf = -node_idx-1;

    EML_LOG_BEGIN("eml-trees-predict-tree-end");
    EML_LOG_ADD_INTEGER("node", node_idx);
    EML_LOG_ADD_INTEGER("leaf", leaf);
    EML_LOG_END();

    return leaf;
}


static inline int32_t
eml_trees_outputs_proba(const EmlTrees *self)
{
    // TODO: consider using only 1 for binary classification?
    return self->n_classes;
}

EmlError
eml_trees_predict_proba(const EmlTrees *self,
            const int16_t *features, int8_t features_length,
            float *out, int32_t out_length)
{
    EML_PRECONDITION(features, EmlUninitialized);
    EML_PRECONDITION(out, EmlUninitialized);
    const int32_t n_outputs = eml_trees_outputs_proba(self);
    EML_PRECONDITION(out_length == n_outputs, EmlSizeMismatch);

    for (int i=0; i<out_length; i++) {
        out[i] = 0.0f;
    }

    const int leaf_bits_per_class = self->leaf_bits;
    if (leaf_bits_per_class == 0) {
        // majority voting. Leaf value is a class number

        for (int32_t i=0; i<self->n_trees; i++) {

            // TODO: support storing the class no directly in the leaf_number
            const int32_t leaf_number = eml_trees_predict_tree(self, self->tree_roots[i], features, features_length);
            const uint8_t *leaf_data = self->leaves + leaf_number;
            const int32_t class_no = *leaf_data;
            out[class_no] += 1.0;
        }


    } else if (leaf_bits_per_class == 8) {
        // soft voting. Tree leaf is a index into leaves table, containing class proportions 

        const int leaf_size = 1*self->n_classes;

        for (int32_t i=0; i<self->n_trees; i++) {

            const int32_t leaf_number = eml_trees_predict_tree(self, self->tree_roots[i], features, features_length);
            const int32_t leaf_offset = leaf_number * leaf_size;
            const uint8_t *leaf_data = self->leaves + leaf_offset;

            for (int class_no=0; class_no<self->n_classes; class_no++) {
                const float class_proportion = leaf_data[class_no] / 255;
                out[class_no] += class_proportion;
            }

#if 0
            fprintf(stderr,
                " predict-proba-tree tree=%d leaf=%d ",
                i, leaf_number, leaf_offset,
            );
#endif

        }

    } else {
        return EmlUnsupported;
    }

    // compute mean
    for (int i=0; i<out_length; i++) {
        out[i] = out[i] / self->n_trees;
    }

    return EmlOk;
}


/**
* \brief Run inference and return most probable class
*
* \param forest EmlTrees instance
* \param features Input data values
* \param features_length Length of input data
*
* \return The class number, or -EmlTreesError on failure
*/
int32_t
eml_trees_predict(const EmlTrees *forest, const int16_t *features, int8_t features_length)
{
    EML_LOG_BEGIN("eml-trees-predict-start");
    EML_LOG_ADD_INTEGER("classes", forest->n_classes);
    EML_LOG_ADD_INTEGER("features-model", forest->n_features);
    EML_LOG_ADD_INTEGER("features-input", features_length);
    EML_LOG_END();

    if (features_length != forest->n_features) {
        return -EmlTreesErrorLength;
    }

    if (forest->n_classes > EMTREES_MAX_CLASSES) {
        return -EmlTreesErrorLength;
    }

    float votes[EMTREES_MAX_CLASSES] = {0};
    const int n_classes = forest->n_classes;
 
   const EmlError err = \
        eml_trees_predict_proba(forest, features, features_length, votes, n_classes);
    if (err != EmlOk) {
        return -EmlTreesUnknownError;
    }

    int32_t most_voted_class = -1;
    float most_voted_value = 0.0;
    for (int32_t i=0; i<n_classes; i++) {
        //printf("votes[%d]: %d\n", i, votes[i]);
        if (votes[i] > most_voted_value) {
            most_voted_class = i;
            most_voted_value = votes[i];
        }
    }

    EML_LOG_BEGIN("eml-trees-predict-end");
    EML_LOG_ADD_INTEGER("trees", forest->n_trees);
    EML_LOG_ADD_ARRAY("votes", votes, n_classes, "%.2f");
    EML_LOG_ADD_INTEGER("class", most_voted_class);
    EML_LOG_END();

    return most_voted_class;
}

#if EML_TREES_REGRESSION_ENABLE

/**
* \brief Run inference and return regression values
*
* \param forest EmlTrees instance
* \param features Input data values
* \param features_length Length of input data
* \param out Buffer to store output
* \param out_length Length of output buffer
*
* \return EmlOk on success, or error on failure
*/
EmlError
eml_trees_regress(const EmlTrees *forest,
        const int16_t *features, int8_t features_length,
        float *out, int8_t out_length)
{

    EML_LOG_BEGIN("eml-trees-regress-start");
    EML_LOG_ADD_INTEGER("trees", forest->n_trees);
    EML_LOG_ADD_INTEGER("features-model", forest->n_features);
    EML_LOG_ADD_INTEGER("features-input", features_length);
    EML_LOG_END();

    if (out_length < 1) {
        return EmlSizeMismatch;
    }

    if (forest->leaf_bits != 32) {
        // only majority vote supported for now
        return EmlUnsupported;
    }

    const int leaf_size = 4;

    float sum = 0;
    for (int32_t i=0; i<forest->n_trees; i++) {
        const int32_t leaf_number = eml_trees_predict_tree(forest, forest->tree_roots[i], features, features_length);        
        const int32_t leaf_offset = leaf_number * leaf_size;
        const float *leaf_data = (float *)(forest->leaves + leaf_offset);
        const float val = *leaf_data;
        sum += val;
    }

    out[0] = sum / forest->n_trees;

    EML_LOG_BEGIN("eml-trees-regress-end");
    EML_LOG_ADD_INTEGER("output", out[0]);
    EML_LOG_END();

    return EmlOk;
}

/**
* \brief Run inference and return single regression value
*
* \param forest EmlTrees instance
* \param features Input data values
* \param features_length Length of input data
*
* \return The output value on success, or NAN on failure
*/
float
eml_trees_regress1(const EmlTrees *forest,
        const int16_t *features, int8_t features_length)
{
    float out[1];
    EmlError err = eml_trees_regress(forest,
        features, features_length,
        out, 1);
    if (err != EmlOk) {    
        return NAN;
    }
    return out[0];
}

#endif // EML_TREES_REGRESSION_ENABLE

#ifdef __cplusplus
}
#endif

#endif // EML_TREES_H
