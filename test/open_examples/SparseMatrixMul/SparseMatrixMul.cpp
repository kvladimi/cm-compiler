/*
 * Copyright (c) 2017, Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <iostream>
#include <memory>
#include <string>

// The only CM runtime header file that you need is cm_rt.h.
// It includes all of the CM runtime.
#include "cm_rt.h"

// Includes cm_rt_helpers.h to convert the integer return code returned from
// the CM runtime to a meaningful string message.
#include "common/cm_rt_helpers.h"

// Includes isa_helpers.h to load the ISA file generated by the CM compiler.
#include "common/isa_helpers.h"

///////////////////////////////////////////////////////////////////////////////
// Configuration parameters for CM host and kernel
///////////////////////////////////////////////////////////////////////////////
#define OWORD_BUF_ALIGNMENT   (4)  // Kernel required alignment for OWORD reads

// The following 3 parameters are for controlling max # of the sparse matrix
// rows processed per enqueue.
// Total number of active h/w threads
//   = MULTIPLIER * THREAD_SPACE_WIDTH
// Total number of sparse matrix rows processed
//   = MULTIPLIER * THREAD_SPACE_WIDTH * ROWS_PER_THREAD
// where
//   THREAD_SPACE_WIDTH corresponds to thread space width
//   MULTIPLIER corresponds to thread space height
//   ROWS_PER_THREAD corresponds to max scatter read capability
#define THREAD_SPACE_WIDTH    (60)
#define MULTIPLIER            (16)
#define ROWS_PER_THREAD       (16)

#define ROUND(value, unit)    \
    (((value) / (unit) + (((value) % (unit)) ? 1: 0)) * (unit))
#define MIN(value1, value2)   ((value1 < value2)? value1: value2)
#define MAX(value1, value2)   ((value1 > value2)? value1: value2)
#define ABS(x)                (((x) < 0)? -(x): (x))

// Structure for storing SparseMatrix in compressed sparse row format.
struct CsrSparseMatrix {
  unsigned num_rows;
  unsigned num_cols;
  unsigned num_nonzeros;
  unsigned *Arow;  // pointer to the extents of rows for a CSR sparse matrix
  unsigned *Acol;  // pointer to the column indices for a CSR sparse matrix
  float *Anz;      // pointer to the non-zero values for a CSR sparse matrix
  ~CsrSparseMatrix()
  {
    delete[] Arow;
    Arow = nullptr;
    delete[] Acol;
    Acol = nullptr;
    delete[] Anz;
    Anz = nullptr;
  }
};

template <typename IndexType, typename ValueType>
void SpmvCsr(
  SurfaceIndex ANZ_BUF,   // non-zero values of Spmv_csr buf index
  SurfaceIndex ACOL_BUF,  // col-indices of Spmv_csr buf index
  SurfaceIndex AROW_BUF,  // extent of rows of Spmv_csr buf index
  SurfaceIndex X_BUF,     // X-vector buf index
  SurfaceIndex Y_BUF,     // Y-vector buf index
  IndexType row_number,   // starting matrix row to process
  short row_stride,       // thread space width
  IndexType max_rows,     // max valid rows of the matrix
  IndexType v_st[ROWS_PER_THREAD] // per scatter read offset locations
);

int ReadCsrFile(const char *csr_filename, CsrSparseMatrix &csr) {
  // This subroutine is to read in a CSR formatted matrix from a file
  // Param csr_filename: is an input file containing Spmv_csr.
  // Param csr: this structure will contain Spmv_csr after this call.

  FILE *f = fopen(csr_filename, "rb");
  if (f == NULL) {
    fprintf(stderr, "Error opening file %s", csr_filename);
    std::exit(1);
  }

  // Reads # cols (unsigned).
  if (fread(&csr.num_cols, sizeof(unsigned), 1, f) != 1) {
    fprintf(stderr, "Error reading num_cols from %s\n", csr_filename);
    std::exit(1);
  }

  // Reads # rows (unsigned).
  if (fread(&csr.num_rows, sizeof(unsigned), 1, f) != 1) {
    fprintf(stderr, "Error reading num_rows from %s\n", csr_filename);
    std::exit(1);
  }

  // Reads # non-zero values (unsigned).
  if (fread(&csr.num_nonzeros, sizeof(unsigned), 1, f) != 1) {
    fprintf(stderr, "Error reading num_nonzeros from %s\n", csr_filename);
    std::exit(1);
  }

  // Reads column indices (unsigned *).
  csr.Acol = new unsigned[csr.num_nonzeros];
  if (fread(csr.Acol, sizeof(unsigned), csr.num_nonzeros, f) !=
      csr.num_nonzeros) {
    fprintf(stderr, "Error reading column indices from %s\n", csr_filename);
    std::exit(1);
  }

  // Reads extent of rows (unsigned *).
  csr.Arow = new unsigned[csr.num_rows + 1];
  if (fread(csr.Arow, sizeof(unsigned), csr.num_rows + 1, f) !=
      csr.num_rows + 1) {
    fprintf(stderr, "Error reading extent of rows from %s\n", csr_filename);
    std::exit(1);
  }

  // Reads all non-zero values (float *).
  csr.Anz = new float[csr.num_nonzeros];
  if (fread(csr.Anz, sizeof(float), csr.num_nonzeros, f) != csr.num_nonzeros) {
    fprintf(stderr, "Error reading non-zeros from %s\n", csr_filename);
    std::exit(1);
  }

  fclose(f);

  return 0;
}

int RunCsrSpmvOnGpu(const CsrSparseMatrix &csr) {
  // This subroutine is for multiplying a sparse matrix with a vector using
  // the GPU
  // Param csr: is an input sparse matrix stored in CSR format.
  // Equation to be performed is as follows:
  //   Y vector = Y vector + csr sparseMatrix * X vector
  // Before performing the above calculation, the subroutine will
  // 1. align the X, Y, csr dimensions to OWORD_BUF_ALIGNMENT
  // 2. initialize X and Y vectors with randomized values generated
  // using the same random seed.
  // In this example, this subroutine will do the above NUM_ITER times with
  // the same initial X and Y vectors, and will compare first Y result with
  // subsequent Y (NUM_ITER - 1) results.

  srand(1);

  unsigned rounded_num_rows = ROUND(csr.num_rows, OWORD_BUF_ALIGNMENT);
  unsigned rounded_num_cols = ROUND(csr.num_cols + 1, OWORD_BUF_ALIGNMENT);

  // Randomizes x and y arrays.  They are aligned to OWORD_BUF_ALIGNMENT.
  float *x = new float[rounded_num_cols];
  float *y = new float[rounded_num_rows];

  x[0] = 0;

  for (unsigned i = 1; i < csr.num_cols + 1; i++) {
    x[i] = static_cast<float>(rand() / (RAND_MAX + 1.0));
  }

  for (unsigned i = csr.num_cols + 1; i < rounded_num_cols; i++) {
    x[i] = 0.0;
  }

  for (unsigned i = 0; i < csr.num_rows; i++) {
    y[i] = static_cast<float>(rand() / (RAND_MAX + 1.0));
  }

  for (unsigned i = csr.num_rows; i < rounded_num_rows; i++) {
    y[i] = 0.0;
  }

  // "ref" will contain reference value computed on CPU.
  float *ref = new float[rounded_num_rows];
  memcpy(ref, y, rounded_num_rows * sizeof(float));

  // compute on cpu
  for (unsigned i = 0; i < csr.num_rows; ++i) {
    for (unsigned k = csr.Arow[i]; k < csr.Arow[i+1]; ++k) {
      ref[i] += csr.Anz[k] * x[csr.Acol[k]+1];
    }
  }

  // Creates aligned version of CSR arrays:
  //   anz for non-zeros
  //   arow for extent of rows
  //   acol for column indices
  // They are aligned to OWORD_BUF_ALIGNMENT.
  unsigned int rounded_anz_length = 0;

  for (unsigned i = 0; i < csr.num_rows; i++) {
    unsigned int row_length = csr.Arow[i + 1] - csr.Arow[i];
    rounded_anz_length += row_length;
  }

  unsigned int *arow = new unsigned int[rounded_num_rows + 1];
  float *anz = new float[rounded_anz_length];
  unsigned *acol = new unsigned[rounded_anz_length];
  arow[0] = 0;

  for (unsigned i = 0; i < csr.num_rows; i++) {
    unsigned row_start = csr.Arow[i];
    unsigned row_end = csr.Arow[i + 1];
    unsigned row_length = row_end - row_start;

    unsigned rounded_row_length = row_length;
    unsigned rounded_row_start = arow[i];
    unsigned rounded_row_end = rounded_row_start + rounded_row_length;

    arow[i + 1] = rounded_row_end;

    for (unsigned j = 0; j < row_length; j++) {
      anz[rounded_row_start] = csr.Anz[row_start];
      acol[rounded_row_start++] = csr.Acol[row_start++] + 1;
    }

    for (unsigned j = row_length; j < rounded_row_length; j++) {
      anz[rounded_row_start] = 0;
      acol[rounded_row_start++] = 0;
    }
  }

  for (unsigned i = csr.num_rows; i < rounded_num_rows; i++) {
    arow[i + 1] = arow[i];
  }

  const int NUM_ITER = 10;

  // Creates 10 copies of y vectors.
  float **y_vec = new float *[NUM_ITER];

  for (int i = 0; i < NUM_ITER; i++) {
    y_vec[i] = new float[rounded_num_rows];
    memcpy(y_vec[i], y, rounded_num_rows * sizeof(float));
  }

  // Creates a CmDevice from scratch.
  // Param device: pointer to the CmDevice object.
  // Param version: CM API version supported by the runtime library.
  CmDevice *device = nullptr;
  unsigned version = 0;
  cm_result_check(::CreateCmDevice(device, version));

  // The file SparseMatrixMul_genx.isa is generated when the kernels in the
  // file SparseMatrixMul_genx.cpp are compiled by the CM compiler. There is a
  // kernel included here, "SpmvCsr".
  // Reads in the virtual ISA from "SparseMatrixMul_genx.isa" to the code
  // buffer.
  std::string isa_code = cm::util::isa::loadFile("SparseMatrixMul_genx.isa");
  if (isa_code.empty()) {
    std::cerr << "Error: empty ISA binary.\n";
    std::exit(1);
  }

  // Creates a CmProgram object consisting of the kernels loaded from the code
  // buffer.
  // Param isa_code.data(): Pointer to the code buffer containing the virtual
  // ISA.
  // Param isa_code.size(): Size in bytes of the code buffer containing the
  // virtual ISA.
  CmProgram *program = nullptr;
  cm_result_check(device->LoadProgram(const_cast<char *>(isa_code.data()),
                                      isa_code.size(),
                                      program));

  // Creates the SpmvCsr kernel.
  // Param program: CM Program from which the kernel is created.
  // Param "SpmvCsr": The kernel name which should be no more than 256 bytes
  // including the null terminator.
  CmKernel *kernel_spmv_csr = nullptr;
  cm_result_check(device->CreateKernel(program,
                                       "SpmvCsr<unsigned int, float>",
                                       kernel_spmv_csr));

  // Initializes the input surface for AROW.
  // Creates a CmBuffer of the specified size in bytes containing extent of
  // rows.
  // Param (rounded_num_rows + 1) * sizeof(float): surface size in bytes
  // Param input_surface_arow: reference to the pointer to the CmBuffer
  CmBuffer *input_surface_arow = nullptr;
  cm_result_check(device->CreateBuffer(
      (rounded_num_rows + 1) * sizeof(float),
      input_surface_arow));

  // Copies system memory content to the input surface using the CPU. The
  // system memory content is the data of the input image. The size of data
  // copied is the size of data in the surface.
  cm_result_check(input_surface_arow->WriteSurface(
       reinterpret_cast<unsigned char *>(arow),
       nullptr));

  // Initializes the input surface for ANZ. This buffer is for non-zeros.
  CmBuffer *input_surface_anz = nullptr;
  cm_result_check(device->CreateBuffer(
      rounded_anz_length * sizeof(float),
      input_surface_anz));
  cm_result_check(input_surface_anz->WriteSurface(
      reinterpret_cast<unsigned char *>(anz),
      nullptr));

  // Initializes the input surface for ACOL. This buffer is for column indices.
  CmBuffer *input_surface_acol = nullptr;
  cm_result_check(device->CreateBuffer(
      rounded_anz_length * sizeof(float),
      input_surface_acol));
  cm_result_check(input_surface_acol->WriteSurface(
      reinterpret_cast<unsigned char *>(acol),
      nullptr));

  // Initializes the input surface for X vector. This buffer is for X vector.
  CmBuffer *input_surface_x = nullptr;
  cm_result_check(device->CreateBuffer(
      rounded_num_cols * sizeof(float),
      input_surface_x));
  cm_result_check(input_surface_x->WriteSurface(
      reinterpret_cast<unsigned char *>(x),
      nullptr));

  // Initializes the input surface for Y vectors.
  // NUM_ITER # of buffers are created for Y vectors.
  CmBuffer *inout_surface_ay[NUM_ITER] = {nullptr};
  for (int i = 0; i < NUM_ITER; i++) {
    cm_result_check(device->CreateBuffer(
        rounded_num_rows * sizeof(float),
        inout_surface_ay[i]));
    cm_result_check(inout_surface_ay[i]->WriteSurface(
        reinterpret_cast<unsigned char *>(y_vec[i]),
        nullptr));
  }

  // When a surface is created by the CmDevice a SurfaceIndex object is
  // created. This object contains an unique index value that is mapped to the
  // surface.
  // Gets the input surface index for ANZ. This is for the non-zero values.
  SurfaceIndex *input_surface_anz_idx = nullptr;
  cm_result_check(input_surface_anz->GetIndex(input_surface_anz_idx));

  // Gets the input surface index for ACOL. This is for the column indices.
  SurfaceIndex *input_surface_acol_idx = nullptr;
  cm_result_check(input_surface_acol->GetIndex(input_surface_acol_idx));

  // Gets the input surface index for AROW. This is for the extent of rows.
  SurfaceIndex *input_surface_arow_idx = nullptr;
  cm_result_check(input_surface_arow->GetIndex(input_surface_arow_idx));

  // Gets the input surface index. This is for the X vector.
  SurfaceIndex *input_surface_x_idx = nullptr;
  cm_result_check(input_surface_x->GetIndex(input_surface_x_idx));

  // Gets the input surface indices for Y vectors. These are for the Y vectors.
  SurfaceIndex *inout_surface_ay_idx[NUM_ITER] = {nullptr};
  for (int i = 0; i < NUM_ITER; i++) {
    cm_result_check(inout_surface_ay[i]->GetIndex(inout_surface_ay_idx[i]));
  }

  // Setup additional kernel input data below.

  // The following 3 parameters defined before are for controlling
  // max # of the sparse matrix rows processed per enqueue.
  //   - THREAD_SPACE_WIDTH corresponds to thread space width
  //   - MULTIPLIER corresponds to thread space height
  //   - ROWS_PER_THREAD corresponds to max scatter read capability
  // v_st contains scattered read offset locations to relative rows per thread.
  unsigned v_st[ROWS_PER_THREAD];
  for (int k = 0; k < ROWS_PER_THREAD; k++) {
    v_st[k] = k * THREAD_SPACE_WIDTH;
  }

  // Total number of active h/w threads per enqueue
  //   = MULTIPLIER * THREAD_SPACE_WIDTH
  // Total number of spacse matrix rows processed per enqueue
  //   = MULTIPLIER * THREAD_SPACE_WIDTH * ROWS_PER_THREAD
  // batch_count indicates how many enqueues are needed to process the
  // input sparse matrix.
  unsigned batch_thread_count = THREAD_SPACE_WIDTH * MULTIPLIER;
  unsigned batch_row_size = batch_thread_count * ROWS_PER_THREAD;
  unsigned batch_count = ROUND(csr.num_rows, batch_row_size) / batch_row_size;

  // An event, "sync_event", is created to track the status of the task.
  // Will be used with enqueue.
  CmEvent *sync_event[NUM_ITER];

  // Creates a CmTask object.
  // The CmTask object is a container for CmKernel pointers. It is used to
  // enqueue the kernels for execution.
  CmTask *task = nullptr;
  cm_result_check(device->CreateTask(task));

  // Adds a CmKernel pointer to CmTask.
  // This task has one kernel, "kernel_spmv_csr".
  cm_result_check(task->AddKernel(kernel_spmv_csr));

  // Creates a task queue.
  // The CmQueue is an in-order queue. Tasks get executed according to the
  // order they are enqueued. The next task does not start execution until the
  // current task finishes.
  CmQueue *queue = nullptr;
  cm_result_check(device->CreateQueue(queue));

  // The number of rows in a matrix may not be a multiple of batch_row_size.
  // Calculates the last batch's actual required batch_thread_count.
  // This value will be stored in last_batch_thread_count.
  unsigned last_batch_thread_count = 0;
  if ((csr.num_rows / batch_row_size) * batch_row_size < csr.num_rows) {
    bool done = false;
    for (unsigned int k = 0; k < MULTIPLIER && done == false; k++) {
      unsigned batch_row_start =
          (csr.num_rows / batch_row_size) * batch_row_size;
      for (unsigned int j = 0; j < THREAD_SPACE_WIDTH; j++) {
        unsigned thread_start_row_index =
            batch_row_start + k * THREAD_SPACE_WIDTH * ROWS_PER_THREAD + j;
        if (thread_start_row_index < csr.num_rows) {
          last_batch_thread_count++;
        } else {
          done = true;
          break;
        }
      }
    }
  } else {
    last_batch_thread_count = batch_thread_count;
  }

  last_batch_thread_count = ROUND(last_batch_thread_count, THREAD_SPACE_WIDTH);

  unsigned thread_count = 0;
  for (int i = 0; i < NUM_ITER; i++) {
  // Does the Y = Y + csr * X NUM_ITER times through the "SpmvCsr" kernel.

    for (unsigned batch_row_start = 0; batch_row_start < csr.num_rows;
        batch_row_start += batch_row_size) {
      // Each CmKernel can be executed by multiple concurrent threads.
      // Since the input matrix could be very big, it may take several batches
      // of enqueues to finish processing the whole kernel.
      // last_batch_thread_count calculated before could be smaller than
      // batch_thread_count, and is used for the last enqueue.
      if (batch_row_start + batch_row_size < csr.num_rows) {
        thread_count = batch_thread_count;
      } else {
        thread_count = last_batch_thread_count;
      }

      // Sets kernel arguments for "SpmvCsr".
      // The first kernel argument is the non-zeros buffer index.
      // The second kernel argument is the column indices buffer index.
      // The third kernel argument is the extent of rows buffer index.
      // The fourth kernel argument is the X vector buffer index.
      // The fifth kernel argument is the Y vector buffer index.
      // The sixth kernel argument indicates the start row of the input matrix
      // to be processed.
      // The seventh kernel argument indicates the thread space width.
      // The eighth kernel argument indicates the max row of the input matrix.
      // The ninth kernel argument corresponds to the scattered read offset
      // locations, corresponding to which rows to be processed.
      cm_result_check(kernel_spmv_csr->SetKernelArg(0,
                                                    sizeof(SurfaceIndex),
                                                    input_surface_anz_idx));
      cm_result_check(kernel_spmv_csr->SetKernelArg(1,
                                                    sizeof(SurfaceIndex),
                                                    input_surface_acol_idx));
      cm_result_check(kernel_spmv_csr->SetKernelArg(2,
                                                    sizeof(SurfaceIndex),
                                                    input_surface_arow_idx));
      cm_result_check(kernel_spmv_csr->SetKernelArg(3,
                                                    sizeof(SurfaceIndex),
                                                    input_surface_x_idx));
      cm_result_check(kernel_spmv_csr->SetKernelArg(4,
                                                    sizeof(SurfaceIndex),
                                                    inout_surface_ay_idx[i]));
      cm_result_check(kernel_spmv_csr->SetKernelArg(5,
                                                    sizeof(batch_row_start),
                                                    &batch_row_start));
      short row_stride = THREAD_SPACE_WIDTH;
      cm_result_check(kernel_spmv_csr->SetKernelArg(6,
                                                    sizeof(short),
                                                    &row_stride));
      unsigned max_rows = csr.num_rows;
      cm_result_check(kernel_spmv_csr->SetKernelArg(7,
                                                    sizeof(unsigned),
                                                    &max_rows));
      cm_result_check(kernel_spmv_csr->SetKernelArg(8,
                                                    sizeof(v_st),
                                                    v_st));

      // Creates a CmThreadSpace object.
      // There are two usage models for the thread space. One is to define the
      // dependency between threads to run in the GPU. The other is to define a
      // thread space where each thread can get a pair of coordinates during
      // kernel execution. For this example, we use the later usage model.
      // In this example, it creates a thread space of "thread_count" with
      // thread space width = THREAD_SPACE_WIDTH and
      // thread space height = thread_count / THREAD_SPACE_WIDTH
      // These threads will be mapped to rows of the input matrix for
      // calculating corresponding row's final Y value.
      CmThreadSpace *pCmThreadSpace = nullptr;
      cm_result_check(device->CreateThreadSpace(
          THREAD_SPACE_WIDTH,
          thread_count / THREAD_SPACE_WIDTH,
          pCmThreadSpace));

      // Launches the task on the GPU. Enqueue is a non-blocking call, i.e. the
      // function returns immediately without waiting for the GPU to start or
      // finish execution of the task. The runtime will query the HW status. If
      // the hardware is not busy, the runtime will submit the task to the
      // driver/HW; otherwise, the runtime will submit the task to the
      // driver/HW at another time.
      // An event, "sync_event[i]", is created to track the status of the task.
      cm_result_check(queue->Enqueue(task, sync_event[i], pCmThreadSpace));
    }
  }

  // Destroys a CmTask object.
  // CmTask will be destroyed when CmDevice is destroyed.
  // Here, the application destroys the CmTask object by itself.
  device->DestroyTask(task);

  // Waits for the task associated with the event to finish execution on the
  // GPU (i.e.the task status is CM_STATUS_FINISHED).This API uses event
  // synchronization between the GPU and CPU in order to reduce the CPU usage
  // and power consumption while waiting. The current process goes to sleep
  // and waits for the notification from the OS when the GPU task finishes.

  DWORD dwTimeOutMs = -1;
  cm_result_check(sync_event[NUM_ITER - 1]->WaitForTaskFinished(dwTimeOutMs));

  // Reads the output surface content to the system memory using the CPU.
  // The size of data copied is the size of data in Surface.
  // It is a blocking call. The function will not return until the copy
  // operation is completed.
  // The dependent event "sync_event" ensures that the reading of the surface
  // will not happen until its state becomes CM_STATUS_FINISHED.
  cm_result_check(inout_surface_ay[0]->ReadSurface((unsigned char *)y_vec[0],
                                                    sync_event[0]));
  for (int i = 1; i < NUM_ITER; i++) {
    // Compares Y[i] vectors with Y[0] vector.
    cm_result_check(inout_surface_ay[i]->ReadSurface((unsigned char *)y_vec[i],
                                                     sync_event[i]));

    unsigned error_count = 0;
    float max_rel_error = 0;
    unsigned int error_index = 0;
    float error_ref = 0;
    float error_res = 0;
    float *ref = y_vec[0];
    float *res = y_vec[i];
    for (unsigned int j = 0; j < csr.num_rows; j++) {
      float rel_error = ABS(ref[j] - res[j]) / MAX(ref[j], res[j]);
      if (max_rel_error < rel_error) {
        max_rel_error = rel_error;
        error_index = j;
        error_ref = ref[j];
        error_res = res[j];
        error_count++;
      }
    }
    if (max_rel_error > 0.002) {
      std::cout << "ERROR: Discrepency in run " << i << "!" << std::endl;
      std::cout << "Max rel error = " << max_rel_error << std::endl;
      std::cout << "Error index = " << error_index << std::endl;
      std::cout << "Error ref = " << error_ref << std::endl;
      std::cout << "Error res = " << error_res << std::endl;
      std::cout << "Error count = " << error_count << std::endl;
    }
  }

  memcpy(y, y_vec[0], rounded_num_rows * sizeof(float));
  delete[] x;
  for (int i = 0; i < NUM_ITER; i++) {
    // Destroys the CmEvent.
    // CmEvent must be destroyed by the user explicitly.
    cm_result_check(queue->DestroyEvent(sync_event[i]));
    delete[] y_vec[i];
  }
  delete[] y_vec;

  // Destroys the CmDevice.
  // Also destroys surfaces, kernels, tasks, thread spaces, and queues that
  // were created using this device instance that have not explicitly been
  // destroyed by calling the respective destroy functions.
  cm_result_check(::DestroyCmDevice(device));

  float *res = y;

  // Compares reference value with final Y value.
  float max_rel_error = 0;
  unsigned int error_index = 0;
  float error_ref = 0;
  float error_res = 0;

  for (unsigned int i = 0; i < csr.num_rows; i++) {
    float rel_error = ABS(ref[i] - res[i]) / MAX(ref[i], res[i]);
    if (max_rel_error < rel_error) {
      max_rel_error = rel_error;
      error_index = i;
      error_ref = ref[i];
      error_res = res[i];
    }
  }

  if (max_rel_error > 0.02) {
    std::cout << "Max rel error = " << max_rel_error << std::endl;
    std::cout << "Error index = " << error_index << std::endl;
    std::cout << "Error ref = " << error_ref << std::endl;
    std::cout << "Error res = " << error_res << std::endl;
    std::cout << "FAILED" << std::endl;
    return 1;
  } else {
    std::cout << "Result matches reference CPU implementation" << std::endl;
    std::cout << "PASSED" << std::endl;
    return 0;
  }
  delete[] ref;
  delete[] res;
}

int main(int argc, char *argv[]) {
  // This program shows the usage of a kernel in one task to perform
  // sparse multiplication using the GPU.
  // The equation to be performed is as follows:
  //   Y = Y + [sparse matrix] * X vector
  // The above equation is performed through this core subroutine:
  //   float *RunCsrSpmvOnGpu(const CsrSparseMatrix &csr)
  // This subroutine takes an input of a sparse matrix with csr format.
  // Internally it will initialize the X and Y vectors with pseudo random
  // numbers generated using 1 as the seed value.
  // RunCsrSpmvOnGpu(csr) compares the final Y vector with the
  // reference values on cpu.

  // By default, use "Protein_csr.dat" as input sparse matrix with csr format.
  const char *csr_filename = "Protein_csr.dat";

  for (int i = 1; i < argc; i++) {
    if (argv[i][0] != '-') {
      printf("argv[%d] %s", i, argv[i]);
      csr_filename = argv[i];
      break;
    } else {
      std::cerr << "Unknown option. Exiting..." << std::endl;
      std::cerr << "Usage: SparseMatrixMul.exe [input_matrix]" << std::endl;
      std::exit(1);
    }
  }

  CsrSparseMatrix csr;
  ReadCsrFile(csr_filename, csr);

  std::cout << "Using " << csr.num_rows << "-by-" << csr.num_cols
            << " matrix with " << csr.num_nonzeros << " nonzero values"
            << std::endl;

  return RunCsrSpmvOnGpu(csr);
}
