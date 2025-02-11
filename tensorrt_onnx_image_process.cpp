﻿#include <algorithm>
#include <assert.h>
#include <cmath>
#include <cuda_runtime_api.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <time.h>

#include "NvInfer.h"
#include "NvOnnxParser.h"
#include "common.h"
using namespace nvinfer1;

static const int INPUT_H = 1800;
static const int INPUT_W = 128;
static const int OUTPUT_SIZE = INPUT_H* INPUT_W;
static Logger gLogger;
static int gUseDLACore{-1};
static const int TIMING_ITERATIONS = 1000;
const char* INPUT_BLOB_NAME = "data";

const char* OUTPUT_BLOB_NAME = "prob";

const std::vector<std::string> directories{"data/samples/mnist/", "data/mnist/"};
std::string locateFile(const std::string& input)
{
    return locateFile(input, directories);
}

// simple PGM (portable greyscale map) reader
void readPGMFile(const std::string& fileName, uint8_t buffer[INPUT_H * INPUT_W])
{
    readPGMFile(fileName, buffer, INPUT_H, INPUT_W);
}

void onnxToTRTModel(const std::string& modelFile, // name of the onnx model
                    unsigned int maxBatchSize,    // batch size - NB must be at least as large as the batch we want to run with
                    IHostMemory*& trtModelStream) // output buffer for the TensorRT model
{
    int verbosity = (int) nvinfer1::ILogger::Severity::kWARNING;

    // create the builder
    IBuilder* builder = createInferBuilder(gLogger);
    nvinfer1::INetworkDefinition* network = builder->createNetwork();

    auto parser = nvonnxparser::createParser(*network, gLogger);

    //Optional - uncomment below lines to view network layer information
    //config->setPrintLayerInfo(true);
    //parser->reportParsingInfo();

    if (!parser->parseFromFile(locateFile(modelFile, directories).c_str(), verbosity))
    {
        string msg("failed to parse onnx file");
        gLogger.log(nvinfer1::ILogger::Severity::kERROR, msg.c_str());
        exit(EXIT_FAILURE);
    }

    // Build the engine
    builder->setMaxBatchSize(maxBatchSize);
    builder->setMaxWorkspaceSize(1 << 20);

    samplesCommon::enableDLA(builder, gUseDLACore);
    ICudaEngine* engine = builder->buildCudaEngine(*network);
    assert(engine);

    // we can destroy the parser
    parser->destroy();

    // serialize the engine, then close everything down
    trtModelStream = engine->serialize();
    engine->destroy();
    network->destroy();
    builder->destroy();
}

void doInference(IExecutionContext& context, float* input, float* output, int batchSize)
{
    const ICudaEngine& engine = context.getEngine();
    // input and output buffer pointers that we pass to the engine - the engine requires exactly IEngine::getNbBindings(),
    // of these, but in this case we know that there is exactly one input and one output.
    assert(engine.getNbBindings() == 2);
    void* buffers[2];

    // In order to bind the buffers, we need to know the names of the input and output tensors.
    // note that indices are guaranteed to be less than IEngine::getNbBindings()
    int inputIndex, outputIndex;
    for (int b = 0; b < engine.getNbBindings(); ++b)
    {
        if (engine.bindingIsInput(b))
            inputIndex = b;
        else
            outputIndex = b;
    }

    // create GPU buffers and a stream
    CHECK(cudaMalloc(&buffers[inputIndex], batchSize * INPUT_H * INPUT_W * sizeof(float)));
    CHECK(cudaMalloc(&buffers[outputIndex], batchSize * OUTPUT_SIZE * sizeof(float)));

    cudaStream_t stream;
    CHECK(cudaStreamCreate(&stream));

    // DMA the input to the GPU,  execute the batch asynchronously, and DMA it back:
    CHECK(cudaMemcpyAsync(buffers[inputIndex], input, batchSize * INPUT_H * INPUT_W * sizeof(float), cudaMemcpyHostToDevice, stream));
    context.enqueue(batchSize, buffers, stream, nullptr);
    CHECK(cudaMemcpyAsync(output, buffers[outputIndex], batchSize * OUTPUT_SIZE * sizeof(float), cudaMemcpyDeviceToHost, stream));
    cudaStreamSynchronize(stream);

    // release the stream and the buffers
    cudaStreamDestroy(stream);
    CHECK(cudaFree(buffers[inputIndex]));
    CHECK(cudaFree(buffers[outputIndex]));
}


int main(int argc, char** argv)
{
    gUseDLACore = samplesCommon::parseDLA(argc, argv);
    // create a TensorRT model from the onnx model and serialize it to a stream
    IHostMemory* trtModelStream{nullptr};
    onnxToTRTModel("beamformer_v7.onnx", 1, trtModelStream);
    assert(trtModelStream != nullptr);



    float* data=new float[INPUT_H * INPUT_W];

	FILE* fd;
	fd = fopen("Ireference_tran.bin", "rb");
	fread(data, INPUT_H * INPUT_W, sizeof(float), fd);
	fclose(fd);

	fd = fopen("beamformed_data_load_test.bin", "wb");
	fwrite(data, INPUT_H * INPUT_W, sizeof(float), fd);
	fclose(fd);

    // deserialize the engine
    IRuntime* runtime = createInferRuntime(gLogger);
    assert(runtime != nullptr);
    if (gUseDLACore >= 0)
    {
        runtime->setDLACore(gUseDLACore);
    }

    ICudaEngine* engine = runtime->deserializeCudaEngine(trtModelStream->data(), trtModelStream->size(), nullptr);
    assert(engine != nullptr);

	printf("Bindings after deserializing:\n");

	for (int bi = 0; bi < engine->getNbBindings(); bi++) {

		if (engine->bindingIsInput(bi) == true) {

			printf("Binding %d (%s): Input.\n", bi, engine->getBindingName(bi));
			printf("getBindingDataType=(%d).\n", engine->getBindingDataType(bi));
		}
		else {

			printf("Binding %d (%s): Output.\n", bi, engine->getBindingName(bi));
			printf("getBindingDataType=(%d).\n", engine->getBindingDataType(bi));
		}

	}

	cout << "layers= " << engine->getNbLayers() << endl;


    trtModelStream->destroy();
    IExecutionContext* context = engine->createExecutionContext();
    assert(context != nullptr);
    // run inference
    float* prob=new float[OUTPUT_SIZE];
    doInference(*context, data, prob, 1);

	fd = fopen("beamformed_data.bin", "wb");
	fwrite(prob, OUTPUT_SIZE, sizeof(float), fd);
	fclose(fd);
    // destroy the engine
    context->destroy();
    engine->destroy();
    runtime->destroy();
	delete[] prob;
	delete []data;
    return EXIT_SUCCESS;
}
