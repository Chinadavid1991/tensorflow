/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");

You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

//
// C++ implementation of Magenta melody basic_rnn with CUDNN RNN
// Author: Rock Zhuang
// Date  : Jan 17, 2019
//

#include <cassert>

#include "tensorflow/cc/client/client_session.h"
#include "tensorflow/cc/ops/cudnn_rnn_ops.h"
#include "tensorflow/cc/ops/dataset_ops_internal.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/cc/training/queue_runner.h"
#include "tensorflow/core/protobuf/queue_runner.pb.h"

using namespace tensorflow;
using namespace tensorflow::ops;
using namespace tensorflow::ops::internal;
using namespace std;

// #define VERBOSE 1
// #define TESTING 1

// Adjustable parameters
#define NUM_LAYERS 1  // For now, only support NUM_LAYERS = 1

// For now, DIR must be 1, TODO.
// dir = (direction == bidirectional) ? 2 : 1
#define DIR 1

#define NUM_UNIT 64   // HIDDEN_SIZE
#define TIME_LEN 20   // NUM_STEPS
#define BATCH_SIZE 4  //
#define TRAINING_STEPS 10000

// Don't change
// (DEFAULT_MAX_NOTE(84) - DEFAULT_MIN_NOTE(48) + NUM_SPECIAL_MELODY_EVENTS(2))
#define INPUT_SIZE 38

#define SEQ_LENGTH TIME_LEN* BATCH_SIZE

#define CUDNN_LSTM_PARAMS_PER_LAYER 8

namespace tensorflow {

class InternalClientSession {
 public:
  static tensorflow::Session* GetSession(tensorflow::ClientSession& session) {
    return session.GetSession();
  }
};

}  // namespace tensorflow

QueueRunnerDef BuildQueueRunnerDef(
    const std::string& queue_name, const std::vector<std::string>& enqueue_ops,
    const std::string& close_op, const std::string& cancel_op,
    const std::vector<tensorflow::error::Code>& queue_closed_error_codes) {
  QueueRunnerDef queue_runner_def;
  *queue_runner_def.mutable_queue_name() = queue_name;
  for (const std::string& enqueue_op : enqueue_ops) {
    *queue_runner_def.mutable_enqueue_op_name()->Add() = enqueue_op;
  }
  *queue_runner_def.mutable_close_op_name() = close_op;
  *queue_runner_def.mutable_cancel_op_name() = cancel_op;
  for (const auto& error_code : queue_closed_error_codes) {
    *queue_runner_def.mutable_queue_closed_exception_types()->Add() =
        error_code;
  }
  return queue_runner_def;
}

int main() {
  // Scope
  Scope root = Scope::NewRootScope();

  //
  // Dataset parsing
  //

  //
  // The file 'training_melodies.tfrecord' was generated by running the
  // following commands from Magenta project: (put midi files into the folder
  // '/tmp/data/midi/' in advance)
  //   $ convert_dir_to_note_sequences --input_dir=/tmp/data/midi/ \
  //       --output_file=/tmp/data/notesequences.tfrecord --recursive
  //   $ melody_rnn_create_dataset --config=basic_rnn \
  //   --input=/tmp/data/notesequences.tfrecord --output_dir=/tmp/data/ \
  //   --eval_ratio=0.0
  //

  // TFRecordDataset
  Tensor filenames("/tmp/data/training_melodies.tfrecord");
  Tensor compression_type("");
  Tensor buffer_size((int64)1024);

  Output tfrecord_dataset =
      TFRecordDataset(root, filenames, compression_type, buffer_size);
  auto shuffle_and_repeat_dataset = ShuffleAndRepeatDataset(
      root, tfrecord_dataset, Cast(root, 5, DT_INT64),   // buffer_size
      Cast(root, 0, DT_INT64), Cast(root, 0, DT_INT64),  // seedX
      Cast(root, 10, DT_INT64),  // count, -1 for infinite repetition
      std::initializer_list<DataType>{DT_STRING},
      std::initializer_list<PartialTensorShape>{{}});
  Output iterator_output =
      Iterator(root, "iterator1", "", vector<DataType>({DT_STRING}),
               vector<PartialTensorShape>({{}}));
  Operation make_iterator_op =
      MakeIterator(root, shuffle_and_repeat_dataset, iterator_output);
  auto iterator_get_next =
      IteratorGetNext(root, iterator_output, vector<DataType>({DT_STRING}),
                      vector<PartialTensorShape>({{}}));

  // Input for ParseExample
  Tensor feature_list_dense_missing_assumed_empty(DT_STRING, TensorShape({0}));

  vector<Output> feature_list_sparse_keys, feature_list_sparse_types,
      feature_list_dense_keys, feature_list_dense_defaults;

  DataTypeSlice feature_list_dense_types = {DT_INT64, DT_FLOAT};
  gtl::ArraySlice<PartialTensorShape> feature_list_dense_shapes = {
      {}, {INPUT_SIZE}};

  vector<Output> context_sparse_keys, context_sparse_types, context_dense_keys,
      context_dense_types, context_dense_defaults, context_dense_shapes;

  feature_list_dense_keys.push_back(
      Const<string>(root, "labels", TensorShape({})));
  feature_list_dense_keys.push_back(
      Const<string>(root, "inputs", TensorShape({})));

  feature_list_dense_defaults.push_back(Const<int64>(root, 2, TensorShape({})));
  feature_list_dense_defaults.push_back(
      Const<float>(root, 1, TensorShape({INPUT_SIZE})));

  // ParseSingleSequenceExample parse only one sequence. ParseSequenceExample
  // supports batch inputs
  auto parse_single_sequence_example = ParseSingleSequenceExample(
      root, iterator_get_next[0], feature_list_dense_missing_assumed_empty,
      InputList(context_sparse_keys), InputList(context_dense_keys),
      InputList(feature_list_sparse_keys), InputList(feature_list_dense_keys),
      InputList(context_dense_defaults),
      Const<string>(root, "melody_rnn_training sequence parsing",
                    TensorShape({})),
      ParseSingleSequenceExample::Attrs()
          .FeatureListDenseTypes(feature_list_dense_types)
          .FeatureListDenseShapes(feature_list_dense_shapes));

  // QueueRunner
  constexpr char kCancelOp[] = "cancel0";
  constexpr char kCloseOp[] = "close0";
  constexpr char kDequeueOp[] = "dequeue0";
  constexpr char kDequeueOp1[] = "dequeue0:1";
  constexpr char kEnqueueOp[] = "enqueue0";
  constexpr char kQueueName[] = "fifoqueue";

  auto pfq = FIFOQueue(root.WithOpName(kQueueName),
                       {DataType::DT_INT64, DataType::DT_FLOAT});
  auto enqueue = QueueEnqueue(
      root.WithOpName(kEnqueueOp), pfq,
      InputList(parse_single_sequence_example.feature_list_dense_values));
  auto closequeue = QueueClose(root.WithOpName(kCloseOp), pfq);
  auto cancelqueue = QueueClose(root.WithOpName(kCancelOp), pfq,
                                QueueClose::CancelPendingEnqueues(true));
  // QueueDequeueMany to deque multiple items as a batch
  auto dequeue = QueueDequeue(root.WithOpName(kDequeueOp), pfq,
                              {DataType::DT_INT64, DataType::DT_FLOAT});

  // Session
  // Note that ClientSession can extend graph before running, Session cannot.
  vector<Tensor> dataset_outputs;
  ClientSession session(root);

  // Run make_iterator_output first
  TF_CHECK_OK(session.Run({}, {}, {make_iterator_op}, nullptr));

  // Coordinator and QueueRunner
  QueueRunnerDef queue_runner =
      BuildQueueRunnerDef(kQueueName, {kEnqueueOp}, kCloseOp, kCancelOp,
                          {tensorflow::error::Code::OUT_OF_RANGE,
                           tensorflow::error::Code::CANCELLED});
  Coordinator coord;
  std::unique_ptr<QueueRunner> qr;
  TF_CHECK_OK(QueueRunner::New(queue_runner, &coord, &qr));
  TF_CHECK_OK(qr->Start(InternalClientSession::GetSession(session)));
  TF_CHECK_OK(coord.RegisterRunner(std::move(qr)));

  while (session
             .Run(RunOptions(), {}, {kDequeueOp, kDequeueOp1}, {},
                  &dataset_outputs)
             .ok()) {
#ifdef VERBOSE
    LOG(INFO) << "Print deque: " << dataset_outputs[0].DebugString() << ", "
              << dataset_outputs[1].DebugString();
    // for(int i = 0; i < dataset_outputs[0].NumElements(); i++) {
    //   LOG(INFO) << "Print labels: " << dataset_outputs[0].vec<int64>()(i);
    // }
#endif
  }
  // For now, only the lastest sequence example is used below

  //
  // Train
  //

  std::vector<Output> weights;
  std::vector<Output> bias;

  //
  // For now, only handle one layer case
  // Ref: cudnn_rnn.py - build
  //
  auto rate = Const(root, {0.01f});

  int num_gates = CUDNN_LSTM_PARAMS_PER_LAYER / 2;

  // wts_applied_on_inputs
  for (int i = 0; i < num_gates; i++) {
    auto v = Variable(root, {INPUT_SIZE, NUM_UNIT}, DT_FLOAT);
    auto rn = RandomNormal(root, {INPUT_SIZE, NUM_UNIT}, DT_FLOAT);
    auto assign_v = Assign(root, v, Multiply(root, rn, rate));

    TF_CHECK_OK(session.Run({assign_v}, nullptr));

    weights.push_back(v);
  }

  // wts_applied_on_hidden_states
  for (int i = 0; i < num_gates; i++) {
    auto v = Variable(root, {NUM_UNIT, NUM_UNIT}, DT_FLOAT);
    auto rn = RandomNormal(root, {NUM_UNIT, NUM_UNIT}, DT_FLOAT);
    auto assign_v = Assign(root, v, Multiply(root, rn, rate));

    TF_CHECK_OK(session.Run({assign_v}, nullptr));

    weights.push_back(v);
  }

  // _canonical_bias_shape
  for (int i = 0; i < CUDNN_LSTM_PARAMS_PER_LAYER * DIR; i++) {
    auto v = Variable(root, {NUM_UNIT}, DT_FLOAT);
    auto rn = RandomNormal(root, {NUM_UNIT}, DT_FLOAT);
    auto assign_v = Assign(root, v, Multiply(root, rn, rate));

    TF_CHECK_OK(session.Run({assign_v}, nullptr));

    bias.push_back(v);
  }

  auto crctp = CudnnRNNCanonicalToParams(root, NUM_LAYERS, NUM_UNIT, INPUT_SIZE,
                                         InputList(weights), InputList(bias));
  LOG(INFO) << "-------------------------------------CudnnRNNCanonicalToParams "
               "status: "
            << root.status();

  // params_size
  vector<Tensor> params_outputs;
  TF_CHECK_OK(session.Run({}, {crctp}, {}, &params_outputs));
  LOG(INFO) << "----------------------------params_outputs: "
            << params_outputs[0].DebugString();

  vector<Tensor> params_size_outputs;
  auto cudnn_RNN_params_size = CudnnRNNParamsSize(
      root, Const<int32>(root, NUM_LAYERS, TensorShape({})),  // num_layers,
      Const<int32>(root, NUM_UNIT, TensorShape({})),          // num_units,
      Const<int32>(root, INPUT_SIZE, TensorShape({})),        // input_size,
      DT_FLOAT, DT_INT32);  // DataType T, DataType S
  LOG(INFO)
      << "-------------------------------------CudnnRNNParamsSize status: "
      << root.status();
  TF_CHECK_OK(
      session.Run({}, {cudnn_RNN_params_size}, {}, &params_size_outputs));
  int param_size = params_size_outputs[0].scalar<int32>()();
  LOG(INFO) << "----------------------------params_size_outputs: "
            << params_size_outputs[0].DebugString()
            << ", param_size: " << param_size;

  // Trainable parameters

  auto params = Variable(root, {param_size}, DT_FLOAT);

#if 1
  auto assign_params = Assign(root, params, params_outputs[0]);
#else
#if 1
  // Random value
  auto random_value = RandomNormal(root, {param_size}, DT_FLOAT);
  auto assign_params = Assign(root, params, Multiply(root, random_value, rate));
#else
  // Zero out
  Tensor params_zero_tensor(DT_FLOAT, TensorShape({param_size}));
  params_zero_tensor.vec<float>().setZero();
  auto assign_params =
      Assign(root, params, ZerosLike(root, params_zero_tensor));
#endif
#endif

  // variables
  auto w_y = Variable(root, {INPUT_SIZE, NUM_UNIT}, DT_FLOAT);
  auto random_value2 = RandomNormal(root, {INPUT_SIZE, NUM_UNIT}, DT_FLOAT);
  auto assign_w_y = Assign(root, w_y, Multiply(root, random_value2, rate));

  auto b_y = Variable(root, {INPUT_SIZE}, DT_FLOAT);
  Tensor b_y_zero_tensor(DT_FLOAT, TensorShape({INPUT_SIZE}));
  b_y_zero_tensor.vec<float>().setZero();
  auto assign_b_y = Assign(root, b_y, ZerosLike(root, b_y_zero_tensor));

  // Gradient accum parameters start here
  auto ada_params = Variable(root, {param_size}, DT_FLOAT);
  Tensor ada_params_zero_tensor(DT_FLOAT, TensorShape({param_size}));
  ada_params_zero_tensor.vec<float>().setZero();
  auto assign_ada_params =
      Assign(root, ada_params, ZerosLike(root, ada_params_zero_tensor));

  auto ada_w_y = Variable(root, {INPUT_SIZE, NUM_UNIT}, DT_FLOAT);
  auto assign_ada_w_y = Assign(root, ada_w_y, ZerosLike(root, w_y));

  auto ada_b_y = Variable(root, {INPUT_SIZE}, DT_FLOAT);
  auto assign_ada_b_y = Assign(root, ada_b_y, ZerosLike(root, b_y));

  // Placeholders
  auto input = Placeholder(
      root, DT_FLOAT, Placeholder::Shape({TIME_LEN, BATCH_SIZE, INPUT_SIZE}));
  auto input_h =
      Placeholder(root, DT_FLOAT,
                  Placeholder::Shape({NUM_LAYERS * DIR, BATCH_SIZE, NUM_UNIT}));
  auto input_c =
      Placeholder(root, DT_FLOAT,
                  Placeholder::Shape({NUM_LAYERS * DIR, BATCH_SIZE, NUM_UNIT}));
  auto output_backprop = Placeholder(
      root, DT_FLOAT, Placeholder::Shape({TIME_LEN, BATCH_SIZE, NUM_UNIT}));
  auto output_h_backprop = Placeholder(
      root, DT_FLOAT,
      Placeholder::Shape(
          {NUM_LAYERS * DIR, BATCH_SIZE,
           NUM_UNIT}));  // [num_layer * dir, batch_size, num_units].
  auto output_c_backprop = Placeholder(
      root, DT_FLOAT,
      Placeholder::Shape({NUM_LAYERS * DIR, BATCH_SIZE,
                          NUM_UNIT}));  // [num_layer * dir, batch, num_units]
  auto y = Placeholder(
      root, DT_INT64,
      Placeholder::Shape({TIME_LEN, BATCH_SIZE}));  // (timelen, batch_size)

  // Cudnn RNN
  auto cudnn_RNN = CudnnRNN(root,
                            input,    // input,
                            input_h,  // input_h,
                            input_c,  // input_c,
                            params);
  LOG(INFO) << "-------------------------------------CudnnRNN status: "
            << root.status();
  auto rnn_softmax_loss_hgrad = RNNSoftmaxLossHGrad(
      root,
      cudnn_RNN.output_h,  // [seq_length, batch_size, dir * num_units]
                           // {timelen, batch_size, cell_size aka. num_units}
      y, w_y, b_y);
  LOG(INFO)
      << "-------------------------------------RNNSoftmaxLossHGrad status: "
      << root.status();
  auto cudnn_RNN_backprop = CudnnRNNBackprop(
      root, input, input_h, input_c, params, cudnn_RNN.output,
      cudnn_RNN.output_h, cudnn_RNN.output_c, output_backprop,
      output_h_backprop,  // ?? rnn_softmax_loss_hgrad.h_grad,  // [seq_length,
                          // batch_size, dir * num_units]
      output_c_backprop, cudnn_RNN.reserve_space);
  LOG(INFO) << "-------------------------------------CudnnRNNBackprop status: "
            << root.status();

  // Gradient
  auto lr = Cast(root, 0.03, DT_FLOAT);

  // alternative of ApplyAdagrad
  auto apply_params = ApplyAdagradTrick(root, params, ada_params, lr,
                                        cudnn_RNN_backprop.params_backprop);
  auto apply_w_y =
      ApplyAdagradTrick(root, w_y, ada_w_y, lr, rnn_softmax_loss_hgrad.dw_y);
  auto apply_b_y =
      ApplyAdagradTrick(root, b_y, ada_b_y, lr, rnn_softmax_loss_hgrad.db_y);

  // Initialize variables
  TF_CHECK_OK(session.Run({assign_params, assign_w_y, assign_b_y}, nullptr));
  TF_CHECK_OK(session.Run({assign_ada_params, assign_ada_w_y, assign_ada_b_y},
                          nullptr));

  // loop
  int step = 0;
  while (step < TRAINING_STEPS) {
    // zeroed out when batch 0
    Tensor input_h_tensor(
        DT_FLOAT, TensorShape({NUM_LAYERS * DIR, BATCH_SIZE, NUM_UNIT}));
    typename TTypes<float, 3>::Tensor input_h_t =
        input_h_tensor.tensor<float, 3>();
    input_h_t.setZero();

    Tensor input_c_tensor(
        DT_FLOAT, TensorShape({NUM_LAYERS * DIR, BATCH_SIZE, NUM_UNIT}));
    typename TTypes<float, 3>::Tensor input_c_t =
        input_c_tensor.tensor<float, 3>();
    input_c_t.setZero();

    Tensor output_backprop_tensor(
        DT_FLOAT, TensorShape({TIME_LEN, BATCH_SIZE, NUM_UNIT}));
    typename TTypes<float, 3>::Tensor output_backprop_t =
        output_backprop_tensor.tensor<float, 3>();
    output_backprop_t.setZero();

    Tensor output_h_backprop_tensor(
        DT_FLOAT, TensorShape({NUM_LAYERS * DIR, BATCH_SIZE, NUM_UNIT}));
    typename TTypes<float, 3>::Tensor output_h_backprop_t =
        output_h_backprop_tensor.tensor<float, 3>();
    output_h_backprop_t.setZero();

    Tensor output_c_backprop_tensor(
        DT_FLOAT, TensorShape({NUM_LAYERS * DIR, BATCH_SIZE, NUM_UNIT}));
    typename TTypes<float, 3>::Tensor output_c_backprop_t =
        output_c_backprop_tensor.tensor<float, 3>();
    output_c_backprop_t.setZero();

    // Train
    {
      // Note that every input batch in BATCH_SIZE is from a different example
      Tensor x_tensor(DT_FLOAT,
                      TensorShape({TIME_LEN, BATCH_SIZE, INPUT_SIZE}));
      {
        auto e_2d = x_tensor.shaped<float, 2>({SEQ_LENGTH, INPUT_SIZE});

        for (int i = 0; i < TIME_LEN; i++) {
          Eigen::DSizes<Eigen::DenseIndex, 2> indices_dataset(i, 0);
          Eigen::DSizes<Eigen::DenseIndex, 2> sizes_dataset(1, INPUT_SIZE);
          Eigen::Tensor<float, 2, Eigen::RowMajor> mat =
              dataset_outputs[1].matrix<float>().slice(indices_dataset,
                                                       sizes_dataset);

          for (int b = 0; b < BATCH_SIZE; b++) {
            // set e_2d
            Eigen::DSizes<Eigen::DenseIndex, 2> indices(i * BATCH_SIZE + b, 0);
            Eigen::DSizes<Eigen::DenseIndex, 2> sizes(1, INPUT_SIZE);
            e_2d.slice(indices, sizes) = mat;
          }
        }
      }
#ifdef VERBOSE
      // LOG(INFO) << "-------------------------------x_tensor: " << std::endl
      // << DetailedDebugString(x_tensor);
#endif
      // y
      Tensor y_tensor(DT_INT64, TensorShape({TIME_LEN, BATCH_SIZE}));
      {
        typename TTypes<int64>::Vec y_t =
            y_tensor.shaped<int64, 1>({SEQ_LENGTH});

        // Prepare y
        for (int i = 0; i < TIME_LEN; i++) {
          int64 label =
              dataset_outputs[0].vec<int64>()(i + 1);  // i + 1 for the next one

          for (int b = 0; b < BATCH_SIZE; b++) {
            y_t(i * BATCH_SIZE + b) = label;
          }
        }
      }
#ifdef VERBOSE
      // LOG(INFO) << "-------------------------------y_tensor: " << std::endl
      // << DetailedDebugString(y_tensor);
#endif

      // Run
      vector<Tensor> outputs;
      TF_CHECK_OK(session.Run({{input, x_tensor},
                               {y, y_tensor},
                               {input_h, input_h_tensor},
                               {input_c, input_c_tensor},
                               {output_backprop, output_backprop_tensor},
                               {output_h_backprop, output_h_backprop_tensor},
                               {output_c_backprop, output_c_backprop_tensor}},
                              {rnn_softmax_loss_hgrad.loss, cudnn_RNN.output,
                               apply_params, apply_w_y, apply_b_y},
                              {}, &outputs));

      if (step % 100 == 0) {
#ifdef VERBOSE
        LOG(INFO) << "Print step: " << step
                  << ", loss: " << outputs[0].DebugString();
#endif
        Eigen::Tensor<float, 0, Eigen::RowMajor> total_loss =
            outputs[0].flat<float>().sum();
        LOG(INFO) << "Print step: " << step << ", total_loss: " << total_loss();
      }
    }

    step++;
  }

  // Update input_h_tensor and input_c_tensor

  // Stop
  TF_CHECK_OK(coord.RequestStop());
  TF_CHECK_OK(coord.Join());

  return 0;
}