// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianGlobalAccumulator.h"
#include "GaussianDataTypes.h"
#include "RHICommandList.h"
#include "RHIResources.h"

void FGaussianGlobalAccumulator::ResizeIfNeeded(FRHICommandListBase& RHICmdList, uint32 NewTotalCount)
{
	// Apply render budget cap: working buffers only need to hold up to MaxRenderBudget splats,
	// not ALL splats. With Nanite LOD compaction, visible count is typically much smaller.
	static IConsoleVariable* CVarBudget = IConsoleManager::Get().FindConsoleVariable(TEXT("gs.MaxRenderBudget"));
	uint32 MaxBudget = (CVarBudget && CVarBudget->GetInt() > 0) ? (uint32)CVarBudget->GetInt() : 0;
	if (MaxBudget > 0 && NewTotalCount > MaxBudget)
	{
		NewTotalCount = MaxBudget;
	}

	if (NewTotalCount <= AllocatedCount)
	{
		return;
	}

	// Add 20% headroom to avoid per-frame reallocation when streaming adds tiles,
	// but never exceed the budget cap.
	uint32 NewAllocatedCount = FMath::Max(NewTotalCount + NewTotalCount / 5, 4096u);
	if (MaxBudget > 0)
	{
		NewAllocatedCount = FMath::Min(NewAllocatedCount, MaxBudget);
	}
	uint32 NewNumTiles = FMath::DivideAndRoundUp(NewAllocatedCount, 1024u);

	UE_LOG(LogTemp, Verbose, TEXT("GaussianGlobalAccumulator: Resizing from %u to %u splats (%u tiles)"),
		AllocatedCount, NewAllocatedCount, NewNumTiles);

	// Release old buffers before reallocation
	Release();

	const uint32 UintStride = sizeof(uint32);
	const uint32 ViewDataStride = sizeof(FGaussianSplatViewData);

	FRHIResourceCreateInfo CreateInfo(TEXT("GlobalViewDataBuffer"));

	// --- ViewData buffer ---
	{
		GlobalViewDataBuffer = RHICmdList.CreateBuffer(NewAllocatedCount * ViewDataStride, BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer, ViewDataStride, ERHIAccess::UAVCompute, CreateInfo);
		GlobalViewDataBufferUAV = RHICmdList.CreateUnorderedAccessView(GlobalViewDataBuffer, FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(ViewDataStride));
		GlobalViewDataBufferSRV = RHICmdList.CreateShaderResourceView(GlobalViewDataBuffer, FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(ViewDataStride));
	}

	// --- Sort distance buffer (primary) ---
	{
		FRHIResourceCreateInfo Info(TEXT("GlobalSortDistanceBuffer"));
		GlobalSortDistanceBuffer = RHICmdList.CreateBuffer(NewAllocatedCount * UintStride, BUF_UnorderedAccess | BUF_StructuredBuffer, UintStride, ERHIAccess::UAVCompute, Info);
		GlobalSortDistanceBufferUAV = RHICmdList.CreateUnorderedAccessView(GlobalSortDistanceBuffer, FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
	}

	// --- Sort distance buffer (alt) ---
	{
		FRHIResourceCreateInfo Info(TEXT("GlobalSortDistanceBufferAlt"));
		GlobalSortDistanceBufferAlt = RHICmdList.CreateBuffer(NewAllocatedCount * UintStride, BUF_UnorderedAccess | BUF_StructuredBuffer, UintStride, ERHIAccess::UAVCompute, Info);
		GlobalSortDistanceBufferAltUAV = RHICmdList.CreateUnorderedAccessView(GlobalSortDistanceBufferAlt, FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
	}

	// --- Sort keys buffer (primary) ---
	{
		FRHIResourceCreateInfo Info(TEXT("GlobalSortKeysBuffer"));
		GlobalSortKeysBuffer = RHICmdList.CreateBuffer(NewAllocatedCount * UintStride, BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer, UintStride, ERHIAccess::UAVCompute, Info);
		GlobalSortKeysBufferUAV = RHICmdList.CreateUnorderedAccessView(GlobalSortKeysBuffer, FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
		GlobalSortKeysBufferSRV = RHICmdList.CreateShaderResourceView(GlobalSortKeysBuffer, FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
	}

	// --- Sort keys buffer (alt) ---
	{
		FRHIResourceCreateInfo Info(TEXT("GlobalSortKeysBufferAlt"));
		GlobalSortKeysBufferAlt = RHICmdList.CreateBuffer(NewAllocatedCount * UintStride, BUF_UnorderedAccess | BUF_StructuredBuffer, UintStride, ERHIAccess::UAVCompute, Info);
		GlobalSortKeysBufferAltUAV = RHICmdList.CreateUnorderedAccessView(GlobalSortKeysBufferAlt, FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
	}

	// --- Radix histogram: 256 * NumTiles ---
	{
		uint32 HistogramCount = 256 * NewNumTiles;
		FRHIResourceCreateInfo Info(TEXT("GlobalRadixHistogramBuffer"));
		GlobalRadixHistogramBuffer = RHICmdList.CreateBuffer(HistogramCount * UintStride, BUF_UnorderedAccess | BUF_StructuredBuffer, UintStride, ERHIAccess::UAVCompute, Info);
		GlobalRadixHistogramBufferUAV = RHICmdList.CreateUnorderedAccessView(GlobalRadixHistogramBuffer, FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
	}

	// --- Radix digit offset: 256 entries ---
	{
		FRHIResourceCreateInfo Info(TEXT("GlobalRadixDigitOffsetBuffer"));
		GlobalRadixDigitOffsetBuffer = RHICmdList.CreateBuffer(256 * UintStride, BUF_UnorderedAccess | BUF_StructuredBuffer, UintStride, ERHIAccess::UAVCompute, Info);
		GlobalRadixDigitOffsetBufferUAV = RHICmdList.CreateUnorderedAccessView(GlobalRadixDigitOffsetBuffer, FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
	}

	// --- SortParams: 2 uints — SRV for non-compaction sort, UAV written by PrefixSumCS ---
	{
		FRHIResourceCreateInfo Info(TEXT("GlobalSortParamsBuffer"));
		GlobalSortParamsBuffer = RHICmdList.CreateBuffer(2 * UintStride, BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer, UintStride, ERHIAccess::UAVCompute, Info);
		GlobalSortParamsBufferSRV = RHICmdList.CreateShaderResourceView(GlobalSortParamsBuffer, FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
		GlobalSortParamsBufferUAV = RHICmdList.CreateUnorderedAccessView(GlobalSortParamsBuffer, FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
	}

	AllocatedCount = NewAllocatedCount;
	AllocatedNumTiles = NewNumTiles;

	// Invalidate cache since buffers changed
	bHasCachedSortData = false;
}

void FGaussianGlobalAccumulator::EnsureCompactionBuffersAllocated(FRHICommandListBase& RHICmdList)
{
	if (bCompactionBuffersAllocated)
	{
		return;
	}

	UE_LOG(LogTemp, Verbose, TEXT("GaussianGlobalAccumulator: Allocating compaction prefix-sum buffers (MaxProxies=%u)"), MAX_PROXY_COUNT);

	const uint32 UintStride = sizeof(uint32);

	// --- GlobalVisibleCountArray: [MAX_PROXY_COUNT] uints ---
	{
		FRHIResourceCreateInfo Info(TEXT("GlobalVisibleCountArrayBuffer"));
		GlobalVisibleCountArrayBuffer = RHICmdList.CreateBuffer(MAX_PROXY_COUNT * UintStride, BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer, UintStride, ERHIAccess::UAVCompute, Info);
		GlobalVisibleCountArrayBufferUAV = RHICmdList.CreateUnorderedAccessView(GlobalVisibleCountArrayBuffer, FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
		GlobalVisibleCountArrayBufferSRV = RHICmdList.CreateShaderResourceView(GlobalVisibleCountArrayBuffer, FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
	}

	// --- GlobalBaseOffsets: [MAX_PROXY_COUNT + 1] uints ---
	{
		FRHIResourceCreateInfo Info(TEXT("GlobalBaseOffsetsBuffer"));
		GlobalBaseOffsetsBuffer = RHICmdList.CreateBuffer((MAX_PROXY_COUNT + 1) * UintStride, BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer, UintStride, ERHIAccess::UAVCompute, Info);
		GlobalBaseOffsetsBufferUAV = RHICmdList.CreateUnorderedAccessView(GlobalBaseOffsetsBuffer, FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
		GlobalBaseOffsetsBufferSRV = RHICmdList.CreateShaderResourceView(GlobalBaseOffsetsBuffer, FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
	}

	// --- GlobalCalcDistIndirectArgs: [3] uints ---
	{
		FRHIResourceCreateInfo Info(TEXT("GlobalCalcDistIndirectArgsBuffer"));
		GlobalCalcDistIndirectArgsBuffer = RHICmdList.CreateBuffer(3 * UintStride, BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer, UintStride, ERHIAccess::UAVCompute, Info);
		GlobalCalcDistIndirectArgsBufferUAV = RHICmdList.CreateUnorderedAccessView(GlobalCalcDistIndirectArgsBuffer, FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
	}

	// --- GlobalSortIndirectArgsGlobal: [3] uints ---
	{
		FRHIResourceCreateInfo Info(TEXT("GlobalSortIndirectArgsGlobalBuffer"));
		GlobalSortIndirectArgsGlobalBuffer = RHICmdList.CreateBuffer(3 * UintStride, BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer, UintStride, ERHIAccess::UAVCompute, Info);
		GlobalSortIndirectArgsGlobalBufferUAV = RHICmdList.CreateUnorderedAccessView(GlobalSortIndirectArgsGlobalBuffer, FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
	}

	// --- GlobalDrawIndirectArgs: [5] uints ---
	{
		FRHIResourceCreateInfo Info(TEXT("GlobalDrawIndirectArgsBuffer"));
		GlobalDrawIndirectArgsBuffer = RHICmdList.CreateBuffer(5 * UintStride, BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer, UintStride, ERHIAccess::UAVCompute, Info);
		GlobalDrawIndirectArgsBufferUAV = RHICmdList.CreateUnorderedAccessView(GlobalDrawIndirectArgsBuffer, FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
	}

	bCompactionBuffersAllocated = true;
}

void FGaussianGlobalAccumulator::Release()
{
	GlobalViewDataBuffer.SafeRelease();
	GlobalViewDataBufferUAV.SafeRelease();
	GlobalViewDataBufferSRV.SafeRelease();

	GlobalSortDistanceBuffer.SafeRelease();
	GlobalSortDistanceBufferUAV.SafeRelease();
	GlobalSortDistanceBufferAlt.SafeRelease();
	GlobalSortDistanceBufferAltUAV.SafeRelease();

	GlobalSortKeysBuffer.SafeRelease();
	GlobalSortKeysBufferUAV.SafeRelease();
	GlobalSortKeysBufferSRV.SafeRelease();
	GlobalSortKeysBufferAlt.SafeRelease();
	GlobalSortKeysBufferAltUAV.SafeRelease();

	GlobalRadixHistogramBuffer.SafeRelease();
	GlobalRadixHistogramBufferUAV.SafeRelease();

	GlobalRadixDigitOffsetBuffer.SafeRelease();
	GlobalRadixDigitOffsetBufferUAV.SafeRelease();

	GlobalSortParamsBuffer.SafeRelease();
	GlobalSortParamsBufferSRV.SafeRelease();
	GlobalSortParamsBufferUAV.SafeRelease();

	// Compaction prefix-sum buffers
	GlobalVisibleCountArrayBuffer.SafeRelease();
	GlobalVisibleCountArrayBufferUAV.SafeRelease();
	GlobalVisibleCountArrayBufferSRV.SafeRelease();

	GlobalBaseOffsetsBuffer.SafeRelease();
	GlobalBaseOffsetsBufferUAV.SafeRelease();
	GlobalBaseOffsetsBufferSRV.SafeRelease();

	GlobalCalcDistIndirectArgsBuffer.SafeRelease();
	GlobalCalcDistIndirectArgsBufferUAV.SafeRelease();

	GlobalSortIndirectArgsGlobalBuffer.SafeRelease();
	GlobalSortIndirectArgsGlobalBufferUAV.SafeRelease();

	GlobalDrawIndirectArgsBuffer.SafeRelease();
	GlobalDrawIndirectArgsBufferUAV.SafeRelease();

	AllocatedCount = 0;
	AllocatedNumTiles = 0;
	bHasCachedSortData = false;
	bCompactionBuffersAllocated = false;
}
