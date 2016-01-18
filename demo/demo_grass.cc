/// The MIT License (MIT)
///
/// Copyright (c) 2015 Kirill Bazhenov
/// Copyright (c) 2015 BitBox, Ltd.
///
/// Permission is hereby granted, free of charge, to any person obtaining a copy
/// of this software and associated documentation files (the "Software"), to deal
/// in the Software without restriction, including without limitation the rights
/// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
/// copies of the Software, and to permit persons to whom the Software is
/// furnished to do so, subject to the following conditions:
///
/// The above copyright notice and this permission notice shall be included in
/// all copies or substantial portions of the Software.
///
/// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
/// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
/// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
/// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
/// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
/// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
/// THE SOFTWARE.
#include "common/app.hh"
#include "common/meshloader.hh"
#include "common/textureloader.hh"

#include <vector>
#include <memory>
#include <string>
#include <stack>
#include <algorithm>

#include <glm/glm.hpp>
#include <glm/ext.hpp>

bool listFiles(std::string path, std::string mask, std::vector<std::string>& files) {
    HANDLE hFind = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAA ffd;
    std::string spec;
    std::stack<std::string> directories;

    directories.push(path);
    files.clear();

    while (!directories.empty()) {
        path = directories.top();
        spec = path + "/" + mask;
        directories.pop();

        hFind = FindFirstFileA(spec.c_str(), &ffd);
        if (hFind == INVALID_HANDLE_VALUE) {
            return false;
        }

        do {
            if (strcmp(ffd.cFileName, ".") != 0 && strcmp(ffd.cFileName, "..") != 0) {
                if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    directories.push(path + "/" + ffd.cFileName);
                } else {
                    files.push_back(path + "/" + ffd.cFileName);
                }
            }
        } while (FindNextFileA(hFind, &ffd) != 0);

        if (GetLastError() != ERROR_NO_MORE_FILES) {
            FindClose(hFind);
            return false;
        }

        FindClose(hFind);
        hFind = INVALID_HANDLE_VALUE;
    }

    return true;
}

struct BoundingBox final
{
    glm::vec4 min;
    glm::vec4 max;
};

struct LogicalMeshBuffer final
{
    uint8_t* data             = nullptr;
    uint32_t dataSize         = 0;
    uint32_t dataFormatStride = 0;
    uint32_t physicalAddress  = 0;
};

struct LogicalMesh final
{
    LogicalMeshBuffer indexBuffer;
    LogicalMeshBuffer vertexBuffer;
    uint32_t          count;

    LogicalMesh() {}
    LogicalMesh(MeshData* data)
    {
        vertexBuffer.data             = reinterpret_cast<uint8_t*>(data->getVertices().data());
        vertexBuffer.dataFormatStride = sizeof(MeshData::Vertex);
        vertexBuffer.dataSize         = static_cast<uint32_t>(data->getVertices().size() * sizeof(MeshData::Vertex));

        indexBuffer.data              = reinterpret_cast<uint8_t*>(data->getIndices().data());
        indexBuffer.dataFormatStride  = sizeof(MeshData::Index);
        indexBuffer.dataSize          = static_cast<uint32_t>(data->getIndices().size() * sizeof(MeshData::Index));

        count = static_cast<uint32_t>(data->getIndices().size());
    }
};

struct PhysicalMeshBuffer final
{
    sgfx::BufferHandle physicalBuffer;
    size_t             physicalDataSize;
    bool               isDirty = false;

    typedef std::vector<LogicalMeshBuffer*> PageArray;
    PageArray allPages;

    PhysicalMeshBuffer() = default;
    inline ~PhysicalMeshBuffer()
    {
        sgfx::releaseBuffer(physicalBuffer);
    }

    inline void allocate(LogicalMeshBuffer* logicalBuffer)
    {
        allPages.push_back(logicalBuffer);
        isDirty = true;
    }

    inline void rebuildPages()
    {
        if (!isDirty) return;

        size_t vfStride = allPages[0]->dataFormatStride;
        physicalDataSize = 0;
        for (LogicalMeshBuffer* logicalBuffer: allPages) // calculate total size
            physicalDataSize += logicalBuffer->dataSize; // TODO: find a better place for this

        size_t numElements = physicalDataSize / vfStride;

        sgfx::releaseBuffer(physicalBuffer);
        physicalBuffer = sgfx::createBuffer(
            sgfx::BufferFlags::CPUWrite | sgfx::BufferFlags::StructuredBuffer,
            nullptr,
            physicalDataSize,
            vfStride
        );

        uint8_t* dataPtr = reinterpret_cast<uint8_t*>(sgfx::mapBuffer(physicalBuffer, sgfx::MapType::Write));
        if (dataPtr != nullptr) {
            uint32_t pageOffset = 0;
            for (size_t i = 0; i < allPages.size(); ++i) {
                LogicalMeshBuffer* logicalBuffer = allPages[i];
                // copy logical data to the mapped physical data
                std::memcpy(dataPtr + pageOffset, logicalBuffer->data, logicalBuffer->dataSize);
                // calculate physical address
                logicalBuffer->physicalAddress = pageOffset / logicalBuffer->dataFormatStride;
                // calculate offset
                pageOffset += logicalBuffer->dataSize;
            }

            sgfx::unmapBuffer(physicalBuffer);
        }

        isDirty = false;
    }
};

struct GrassObject final
{
    struct ConstantBuffer
    {
        uint32_t    internalData[4];
        float       mvp[16];
        float       boundingBoxMin[4];
        float       boundingBoxMax[4];
    };

    enum
    {
        kConstantBufferSize = sizeof(ConstantBuffer)
    };
    static_assert((kConstantBufferSize % 16) == 0, "Error: wrong buffer size");

    glm::vec3           position;
    BoundingBox         boundingBox;

    LogicalMeshBuffer*  vertexBuffer;
    LogicalMeshBuffer*  indexBuffer;

    uint32_t            count;

    GrassObject() {}

    GrassObject(LogicalMesh* data)
    {
        vertexBuffer = &data->vertexBuffer;
        indexBuffer  = &data->indexBuffer;
        count        = data->count;

        buildBoundingBox();
    }

    ~GrassObject()
    {}

    void buildBoundingBox()
    {
        glm::vec4 min = glm::vec4(0, 0, 0, 1);
        glm::vec4 max = glm::vec4(0, 0, 0, 1);

        MeshData::Vertex* vertices = reinterpret_cast<MeshData::Vertex*>(vertexBuffer->data);
        for (size_t i = 0; i < (vertexBuffer->dataSize / vertexBuffer->dataFormatStride); ++i) {
            min.x = std::min(min.x, vertices[i].position[0]);
            min.y = std::min(min.y, vertices[i].position[1]);
            min.z = std::min(min.z, vertices[i].position[2]);

            max.x = std::max(max.x, vertices[i].position[0]);
            max.y = std::max(max.y, vertices[i].position[1]);
            max.z = std::max(max.z, vertices[i].position[2]);
        }

        boundingBox.min = min;
        boundingBox.max = max;
    }
};

struct DVPGrassManager final
{
    std::vector<MeshData*>      loadedMeshes;
    std::vector<LogicalMesh*>   logicalMeshes;
    std::vector<GrassObject>    grassObjects;
    PhysicalMeshBuffer          physicalVertexBuffer;
    PhysicalMeshBuffer          physicalIndexBuffer;

    sgfx::SamplerStateHandle    samplerState;
    sgfx::Texture2DHandle       grassTexture;

    sgfx::BufferHandle          initialInstanceBuffer;
    sgfx::BufferHandle          finalInstanceBuffer;
    sgfx::BufferHandle          indirectArgsBuffer;
    uint32_t                    numInstances = 0;

    struct CullCSConstantBuffer
    {
        uint32_t    numItems;
        uint32_t    maxDrawCallCount;
        uint32_t    reserved[2];
    } cullCSConstantData;
    sgfx::ConstantBufferHandle cullCSConstantBuffer;

    enum
    {
        kObjectSizeSquared = 64
    };

    DVPGrassManager()
    {
        sgfx::SamplerStateDescriptor samplerDesc;
        samplerDesc.filter         = sgfx::TextureFilter::MinMagMip_Linear;
        samplerDesc.addressU       = sgfx::AddressMode::Clamp;
        samplerDesc.addressV       = sgfx::AddressMode::Clamp;
        samplerDesc.addressW       = sgfx::AddressMode::Clamp;
        samplerDesc.lodBias        = 0.0F;
        samplerDesc.maxAnisotropy  = 1;
        samplerDesc.comparisonFunc = sgfx::ComparisonFunc::Never;
        samplerDesc.borderColor    = 0xFFFFFFFF;
        samplerDesc.minLod         = -3.402823466e+38F;
        samplerDesc.maxLod         = 3.402823466e+38F;

        samplerState = sgfx::createSamplerState(samplerDesc);
    }

    ~DVPGrassManager()
    {
        for (MeshData* data: loadedMeshes)
            delete data;
        for (LogicalMesh* mesh: logicalMeshes) {
            delete mesh; // TODO: release
        }
        sgfx::releaseSamplerState(samplerState);
        sgfx::releaseTexture(grassTexture);
        sgfx::releaseBuffer(initialInstanceBuffer);
        sgfx::releaseBuffer(finalInstanceBuffer);
        sgfx::releaseBuffer(indirectArgsBuffer);
        sgfx::releaseConstantBuffer(cullCSConstantBuffer);
    }

    void loadTexture(const std::string& path)
    {
        sgfx::releaseTexture(grassTexture);
        grassTexture = loadDDS(path);
    }

    void loadDataSet(const std::string& path)
    {
        std::vector<std::string> files;
        if (listFiles(path, "*.mesh", files)) {

            for (const std::string& file: files) {
                MeshData* data = new MeshData;
                data->read(file);
                loadedMeshes.push_back(data);

                LogicalMesh* logicalMesh = new LogicalMesh(data);
                physicalVertexBuffer.allocate(&logicalMesh->vertexBuffer);
                physicalIndexBuffer.allocate(&logicalMesh->indexBuffer);
                logicalMeshes.push_back(logicalMesh);
            }

            srand(GetTickCount());
            size_t meshCounter = 0;
            for (int i = -kObjectSizeSquared; i < kObjectSizeSquared; ++i) {
                for (int j = -kObjectSizeSquared; j < kObjectSizeSquared; ++j) {

                    float rx = static_cast<float>(rand() % 10);
                    float ry = static_cast<float>(rand() % 10);

                    float fx = static_cast<float>(i) * 3.0F + rx;
                    float fy = static_cast<float>(j) * 3.0F + ry;

                    GrassObject obj(logicalMeshes[meshCounter]);
                    obj.position = glm::vec3(fx, 0.0F, fy);
                    grassObjects.push_back(obj);

                    meshCounter++;
                    if (meshCounter == logicalMeshes.size())
                        meshCounter = 0;
                }
            }
        }
    }

    void render(sgfx::DrawQueueHandle drawQueue, sgfx::ComputeQueueHandle computeQueue, const glm::mat4& mvp)
    {
        physicalVertexBuffer.rebuildPages();
        physicalIndexBuffer.rebuildPages();

        if (grassObjects.size() != numInstances) {
            numInstances = static_cast<uint32_t>(grassObjects.size());

            sgfx::releaseBuffer(initialInstanceBuffer);
            initialInstanceBuffer = sgfx::createBuffer(
                sgfx::BufferFlags::CPUWrite | sgfx::BufferFlags::StructuredBuffer,
                nullptr,
                numInstances * GrassObject::kConstantBufferSize,
                GrassObject::kConstantBufferSize
            );

            sgfx::releaseBuffer(finalInstanceBuffer);
            finalInstanceBuffer = sgfx::createBuffer(
                sgfx::BufferFlags::GPUWrite | sgfx::BufferFlags::GPUCounter | sgfx::BufferFlags::StructuredBuffer | sgfx::BufferFlags::GPUCounter,
                nullptr,
                numInstances * GrassObject::kConstantBufferSize,
                GrassObject::kConstantBufferSize
            );

            sgfx::releaseBuffer(indirectArgsBuffer);
            indirectArgsBuffer = sgfx::createBuffer(
                sgfx::BufferFlags::GPUWrite | sgfx::BufferFlags::IndirectArgs,
                nullptr,
                4 * sizeof(uint32_t),
                4 * sizeof(uint32_t)
            );

            sgfx::releaseConstantBuffer(cullCSConstantBuffer);
            cullCSConstantBuffer = sgfx::createConstantBuffer(&cullCSConstantData, sizeof(CullCSConstantBuffer));
        }

        // update constants
        uint32_t maxDrawCallCount = 0;
        uint8_t* dataPtr = reinterpret_cast<uint8_t*>(sgfx::mapBuffer(initialInstanceBuffer, sgfx::MapType::Write));
        if (dataPtr != nullptr) {

            for (size_t i = 0; i < numInstances; ++i) {
                size_t offset = i * GrassObject::kConstantBufferSize;
                const GrassObject& obj = grassObjects[i];

                glm::mat4 matrix = mvp * glm::translate(obj.position);

                GrassObject::ConstantBuffer constants;
                // fill internal data structure
                constants.internalData[0] = obj.vertexBuffer->physicalAddress;
                constants.internalData[1] = obj.indexBuffer->physicalAddress;

                constants.internalData[2] = 1; // draw indexed
                constants.internalData[3] = obj.count;

                // copy matrix
                std::memcpy(constants.mvp,              glm::value_ptr(matrix),                 sizeof(constants.mvp));
                std::memcpy(constants.boundingBoxMin,   glm::value_ptr(obj.boundingBox.min),    sizeof(constants.boundingBoxMin));
                std::memcpy(constants.boundingBoxMax,   glm::value_ptr(obj.boundingBox.max),    sizeof(constants.boundingBoxMax));

                std::memcpy(dataPtr + offset, &constants, GrassObject::kConstantBufferSize);

                maxDrawCallCount = std::max(maxDrawCallCount, obj.count);
            }

            sgfx::unmapBuffer(initialInstanceBuffer);
        }

        // update const buffer
        {
            cullCSConstantData.numItems         = numInstances;
            cullCSConstantData.maxDrawCallCount = maxDrawCallCount;
            sgfx::updateConstantBuffer(cullCSConstantBuffer, &cullCSConstantData);
        }

        // culling
        {
            sgfx::setConstantBuffer(computeQueue, 0, cullCSConstantBuffer);
            sgfx::setResource(computeQueue, 0, initialInstanceBuffer);
            sgfx::setResourceRW(computeQueue, 0, finalInstanceBuffer);
            sgfx::setResourceRW(computeQueue, 1, indirectArgsBuffer);

            sgfx::submit(computeQueue, numInstances / 128, 1, 1);
        }

        // rendering
        sgfx::setSamplerState(drawQueue, 0, samplerState);
        {
            sgfx::setPrimitiveTopology(drawQueue, sgfx::PrimitiveTopology::TriangleList);
            sgfx::setResource(drawQueue, 0, physicalVertexBuffer.physicalBuffer);
            sgfx::setResource(drawQueue, 1, physicalIndexBuffer.physicalBuffer);
            sgfx::setResource(drawQueue, 2, finalInstanceBuffer);
            sgfx::setResource(drawQueue, 3, grassTexture);
            sgfx::drawInstancedIndirect(drawQueue, indirectArgsBuffer, 0);

            sgfx::submit(drawQueue);
        }

        // clear buffer counter
        {
            sgfx::clearBufferRW(finalInstanceBuffer, 0U);
        }
    }
};

class GrassApplication : public Application
{
public:

    sgfx::VertexShaderHandle  vsHandle;
    sgfx::PixelShaderHandle   psHandle;
    sgfx::SurfaceShaderHandle ssHandle;

    sgfx::PipelineStateHandle pipelineState;
    sgfx::DrawQueueHandle     drawQueue;

    sgfx::Texture2DHandle     colorBuffer;
    sgfx::Texture2DHandle     depthStencilBuffer;
    sgfx::RenderTargetHandle  renderTarget;

    // CS culling
    sgfx::ComputeShaderHandle instanceCullShader;
    sgfx::ComputeQueueHandle  instanceCullQueue;

    DVPGrassManager*          grassManager;

public:

    void* operator new(size_t size)
    {
        return _mm_malloc(size, 32);
    }

    void operator delete(void* ptr)
    {
        _mm_free(ptr);
    }

    virtual void loadSampleData() override
    {
        // setup Sigrlinn
        sgfx::initD3D11(g_pd3dDevice, g_pImmediateContext, g_pSwapChain);

        // create render target
        colorBuffer        = sgfx::getBackBuffer();
        depthStencilBuffer = sgfx::createTexture2D(
            width, height, sgfx::DataFormat::D24S8, 1, sgfx::TextureFlags::DepthStencil
        );

        sgfx::RenderTargetDescriptor renderTargetDesc;
        renderTargetDesc.numColorTextures    = 1;
        renderTargetDesc.colorTextures[0]    = colorBuffer;
        renderTargetDesc.depthStencilTexture = depthStencilBuffer;

        renderTarget = sgfx::createRenderTarget(renderTargetDesc);

        // mesh data
        grassManager = new DVPGrassManager;
        grassManager->loadDataSet("data/meshes/cattail/");
        grassManager->loadTexture("data/textures/CattailBlades.dds");

        vsHandle = loadVS("shaders/dvp.hlsl");
        psHandle = loadPS("shaders/dvp.hlsl");

        if (vsHandle != sgfx::VertexShaderHandle::invalidHandle() && psHandle != sgfx::PixelShaderHandle::invalidHandle()) {
            ssHandle = sgfx::linkSurfaceShader(
                vsHandle,
                sgfx::HullShaderHandle::invalidHandle(),
                sgfx::DomainShaderHandle::invalidHandle(),
                sgfx::GeometryShaderHandle::invalidHandle(),
                psHandle
            );
        }

        if (ssHandle != sgfx::SurfaceShaderHandle::invalidHandle()) {
            sgfx::PipelineStateDescriptor desc;

            desc.rasterizerState.fillMode                           = sgfx::FillMode::Solid;
            desc.rasterizerState.cullMode                           = sgfx::CullMode::None;
            desc.rasterizerState.counterDirection                   = sgfx::CounterDirection::CW;

            desc.blendState.blendDesc.blendEnabled                  = false;
            desc.blendState.blendDesc.writeMask                     = sgfx::ColorWriteMask::All;
            desc.blendState.blendDesc.srcBlend                      = sgfx::BlendFactor::One;
            desc.blendState.blendDesc.dstBlend                      = sgfx::BlendFactor::Zero;
            desc.blendState.blendDesc.blendOp                       = sgfx::BlendOp::Add;
            desc.blendState.blendDesc.srcBlendAlpha                 = sgfx::BlendFactor::One;
            desc.blendState.blendDesc.dstBlendAlpha                 = sgfx::BlendFactor::Zero;
            desc.blendState.blendDesc.blendOpAlpha                  = sgfx::BlendOp::Add;

            desc.depthStencilState.depthEnabled                     = true;
            desc.depthStencilState.writeMask                        = sgfx::DepthWriteMask::All;
            desc.depthStencilState.depthFunc                        = sgfx::DepthFunc::Less;

            desc.depthStencilState.stencilEnabled                   = false;
            desc.depthStencilState.stencilRef                       = 0;
            desc.depthStencilState.stencilReadMask                  = 0;
            desc.depthStencilState.stencilWriteMask                 = 0;

            desc.depthStencilState.frontFaceStencilDesc.stencilFunc = sgfx::StencilFunc::Always;
            desc.depthStencilState.frontFaceStencilDesc.failOp      = sgfx::StencilOp::Keep;
            desc.depthStencilState.frontFaceStencilDesc.depthFailOp = sgfx::StencilOp::Keep;
            desc.depthStencilState.frontFaceStencilDesc.passOp      = sgfx::StencilOp::Keep;

            desc.depthStencilState.backFaceStencilDesc.stencilFunc  = sgfx::StencilFunc::Always;
            desc.depthStencilState.backFaceStencilDesc.failOp       = sgfx::StencilOp::Keep;
            desc.depthStencilState.backFaceStencilDesc.depthFailOp  = sgfx::StencilOp::Keep;
            desc.depthStencilState.backFaceStencilDesc.passOp       = sgfx::StencilOp::Keep;

            desc.shader       = ssHandle;
            desc.vertexFormat = sgfx::VertexFormatHandle::invalidHandle();

            pipelineState = sgfx::createPipelineState(desc);
            if (pipelineState != sgfx::PipelineStateHandle::invalidHandle()) {
                drawQueue = sgfx::createDrawQueue(pipelineState);
            } else {
                OutputDebugString("Failed to create pipeline state!");
            }
        }

        // compute shaders
        instanceCullShader  = loadCS("shaders/dvp_cull.hlsl");
        instanceCullQueue   = sgfx::createComputeQueue(instanceCullShader);
    }

    virtual void releaseSampleData() override
    {
        OutputDebugString("Cleanup\n");
        sgfx::releaseVertexShader(vsHandle);
        sgfx::releasePixelShader(psHandle);
        sgfx::releaseSurfaceShader(ssHandle);
        sgfx::releasePipelineState(pipelineState);
        sgfx::releaseDrawQueue(drawQueue);

        sgfx::releaseComputeShader(instanceCullShader);
        sgfx::releaseComputeQueue(instanceCullQueue);

        sgfx::releaseRenderTarget(renderTarget);
        sgfx::releaseTexture(colorBuffer);
        sgfx::releaseTexture(depthStencilBuffer);

        delete grassManager;

        sgfx::shutdown();
    }

    virtual void renderSample() override
    {
        // Update our time
        static float t = 0.0f;
        static glm::vec3 cameraPosition = glm::vec3(0.0F, 4.0F, -200);

        static ULONGLONG dwTimeStart = 0;
        ULONGLONG dwTimeCur = GetTickCount64();
        if (dwTimeStart == 0)
            dwTimeStart = dwTimeCur;
        t = (dwTimeCur - dwTimeStart) / 1000.0f;
        dwTimeStart = dwTimeCur;

        static bool dtReport = false;
        if (GetAsyncKeyState(VK_F1)) dtReport = !dtReport;

        if (dtReport) {
            char buf[64];
            sprintf_s(buf, "DT: %f\n", t);
            OutputDebugString(buf);
        }
        cameraPosition.z += t * 2.0F;

        glm::mat4 projection = glm::perspective(glm::pi<float>() / 2.0F, width / (FLOAT)height, 0.01f, 50000.0f);
        glm::mat4 view       = glm::lookAt(cameraPosition, cameraPosition + glm::vec3(0.0F, 0.0F, 1.0F), glm::vec3(0.0F, 1.0F, 0.0F));

        glm::mat4 mvp = projection * view;

        // actually draw some stuff
        {
            sgfx::clearRenderTarget(renderTarget, 0xFFFFFFF);
            sgfx::clearDepthStencil(renderTarget, 1.0F, 0);
            sgfx::setRenderTarget(renderTarget);
            sgfx::setViewport(width, height, 0.0F, 1.0F);

            grassManager->render(drawQueue, instanceCullQueue, mvp);
        }
        sgfx::present(1);
    }
};

void sampleApplicationMain()
{
    ApplicationInstance = new GrassApplication;
}
