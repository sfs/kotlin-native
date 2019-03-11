/*
 * Copyright 2010-2019 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */
#include <CoverageMappingC.h>

#include "CoverageMappingC.h"

#include <llvm/ProfileData/Coverage/CoverageMapping.h>
#include <llvm/ProfileData/Coverage/CoverageMappingWriter.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/Triple.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Transforms/Instrumentation.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <string>
#include <vector>
#include <iostream>
#include <iomanip>

using namespace llvm;
using namespace llvm::coverage;

static coverage::CounterMappingRegion::RegionKind determineRegionKind(const struct LLVMCoverageRegion& region) {
    switch (region.kind) {
        case LLVMCoverageRegionKind::CODE:
            return coverage::CounterMappingRegion::RegionKind::CodeRegion;
        case LLVMCoverageRegionKind::GAP:
            return coverage::CounterMappingRegion::RegionKind::GapRegion;
        case LLVMCoverageRegionKind::EXPANSION:
            return coverage::CounterMappingRegion::RegionKind::ExpansionRegion;
    }
}

static coverage::CounterMappingRegion createCounterMappingRegion(struct LLVMCoverageRegion& region) {
    auto regionKind = determineRegionKind(region);
    int expandedFileId = 0;
    if (regionKind == coverage::CounterMappingRegion::RegionKind::ExpansionRegion) {
        expandedFileId = region.expandedFileId;
    }
    const Counter &counter = coverage::Counter::getCounter(region.counterId);
    return coverage::CounterMappingRegion(counter, region.fileId, expandedFileId, region.lineStart,
                                                        region.columnStart, region.lineEnd, region.columnEnd, regionKind);
}

LLVMFunctionCoverage LLVMWriteCoverageRegionMapping(unsigned int *fileIdMapping, size_t fileIdMappingSize,
                                                struct LLVMCoverageRegion **mappingRegions, size_t mappingRegionsSize) {
    std::vector<coverage::CounterMappingRegion> counterMappingRegions;
    for (size_t i = 0; i < mappingRegionsSize; ++i) {
        struct LLVMCoverageRegion region = *mappingRegions[i];
        counterMappingRegions.emplace_back(createCounterMappingRegion(region));
    }
    CoverageMappingWriter writer(ArrayRef<unsigned int>(fileIdMapping, fileIdMappingSize), None, counterMappingRegions);
    std::string CoverageMapping;
    raw_string_ostream OS(CoverageMapping);
    writer.write(OS);
    OS.flush();
    char *coverageData = (char *) malloc(sizeof(char) * (CoverageMapping.length() + 1));
    // Use memcpy because CoverageMapping could contain '\0' in it.
    std::memcpy(coverageData, CoverageMapping.c_str(), CoverageMapping.length());
    return LLVMFunctionCoverage{.coverageData = coverageData, .size = CoverageMapping.length()};
}

void LLVMFunctionCoverageDispose(struct LLVMFunctionCoverage* functionCoverage) {
    delete[] functionCoverage->coverageData;
}

static StructType *getFunctionRecordTy(LLVMContext &Ctx) {
#define COVMAP_FUNC_RECORD(Type, LLVMType, Name, Init) LLVMType,
    Type *FunctionRecordTypes[] = {
#include "llvm/ProfileData/InstrProfData.inc"
    };
    StructType *FunctionRecordTy = StructType::get(Ctx, makeArrayRef(FunctionRecordTypes), true);
    return FunctionRecordTy;
}

static llvm::Constant *addFunctionMappingRecord(llvm::LLVMContext &Ctx, StringRef NameValue, uint64_t FuncHash,
                                                const std::string &CoverageMapping) {
    llvm::StructType *FunctionRecordTy = getFunctionRecordTy(Ctx);

#define COVMAP_FUNC_RECORD(Type, LLVMType, Name, Init) Init,
    llvm::Constant *FunctionRecordVals[] = {
#include "llvm/ProfileData/InstrProfData.inc"
    };
    return llvm::ConstantStruct::get(FunctionRecordTy, makeArrayRef(FunctionRecordVals));
}

// See https://github.com/llvm/llvm-project/blob/fa8fa044ec46b94e64971efa8852df0d58114062/clang/lib/CodeGen/CoverageMappingGen.cpp#L1284.
LLVMValueRef LLVMAddFunctionMappingRecord(LLVMContextRef context, const char *name, uint64_t hash,
                             struct LLVMFunctionCoverage coverageMapping) {
    return llvm::wrap(addFunctionMappingRecord(*llvm::unwrap(context), name, hash,
                                               std::string{coverageMapping.coverageData, coverageMapping.size}));
}

// See https://github.com/llvm/llvm-project/blob/fa8fa044ec46b94e64971efa8852df0d58114062/clang/lib/CodeGen/CoverageMappingGen.cpp#L1335.
// Please note that llvm/ProfileData/InstrProfData.inc refers to variable names of the function that includes it. So be careful with renaming.
static llvm::GlobalVariable* emitCoverageGlobal(
        llvm::LLVMContext &Ctx,
        llvm::Module &module,
        std::vector<llvm::Constant *> &FunctionRecords,
        const llvm::SmallVector<StringRef, 16> &FilenameRefs,
        const std::string &RawCoverageMappings) {

    auto *Int32Ty = llvm::Type::getInt32Ty(Ctx);

    std::string FilenamesAndCoverageMappings;
    llvm::raw_string_ostream outputStream(FilenamesAndCoverageMappings);
    CoverageFilenamesSectionWriter(FilenameRefs).write(outputStream);
    outputStream << RawCoverageMappings;
    size_t CoverageMappingSize = RawCoverageMappings.size();
    size_t FilenamesSize = outputStream.str().size() - CoverageMappingSize;

    // See https://llvm.org/docs/CoverageMappingFormat.html#llvm-ir-representation
    //
    // > Coverage mapping data which is an array of bytes. Zero paddings are added at the end to force 8 byte alignment.
    //
    if (size_t rem = outputStream.str().size() % 8) {
        CoverageMappingSize += 8 - rem;
        for (size_t i = 0; i < 8 - rem; ++i) {
            outputStream << '\0';
        }
    }

    StructType *functionRecordTy = getFunctionRecordTy(Ctx);
    // Create the deferred function records array
    auto functionRecordsTy = llvm::ArrayType::get(functionRecordTy, FunctionRecords.size());
    auto functionRecordsVal = llvm::ConstantArray::get(functionRecordsTy, FunctionRecords);

    llvm::Type *CovDataHeaderTypes[] = {
#define COVMAP_HEADER(Type, LLVMType, Name, Init) LLVMType,

#include "llvm/ProfileData/InstrProfData.inc"
    };
    auto CovDataHeaderTy = llvm::StructType::get(Ctx, makeArrayRef(CovDataHeaderTypes));
    llvm::Constant *CovDataHeaderVals[] = {
#define COVMAP_HEADER(Type, LLVMType, Name, Init) Init,

#include "llvm/ProfileData/InstrProfData.inc"
    };
    auto CovDataHeaderVal = llvm::ConstantStruct::get(CovDataHeaderTy, makeArrayRef(CovDataHeaderVals));

    auto *filenamesAndMappingsVal = llvm::ConstantDataArray::getString(Ctx, outputStream.str(), false);
    // Create the coverage data record
    llvm::Type *CovDataTypes[] = {CovDataHeaderTy, functionRecordsTy, filenamesAndMappingsVal->getType()};
    auto CovDataTy = llvm::StructType::get(Ctx, makeArrayRef(CovDataTypes));

    llvm::Constant *TUDataVals[] = {CovDataHeaderVal, functionRecordsVal, filenamesAndMappingsVal};
    auto CovDataVal = llvm::ConstantStruct::get(CovDataTy, makeArrayRef(TUDataVals));

    return new llvm::GlobalVariable(module, CovDataTy, true, llvm::GlobalValue::InternalLinkage,
            CovDataVal, llvm::getCoverageMappingVarName());
}

static std::string createRawCoverageMapping(struct LLVMFunctionCoverage **functionCoverages, size_t functionCoveragesSize) {
    std::vector<std::string> coverageMappings;
    for (size_t i = 0; i < functionCoveragesSize; ++i) {
        coverageMappings.emplace_back(std::string{(*functionCoverages[i]).coverageData, (*functionCoverages[i]).size});
    }
    return llvm::join(coverageMappings.begin(), coverageMappings.end(), "");
}

LLVMValueRef LLVMCoverageEmit(LLVMModuleRef moduleRef,
        LLVMValueRef *records, size_t recordsSize,
        const char **filenames, int *filenamesIndices, size_t filenamesSize,
        struct LLVMFunctionCoverage **functionCoverages, size_t functionCoveragesSize) {
    LLVMContext &ctx = *unwrap(LLVMGetModuleContext(moduleRef));
    Module &module = *unwrap(moduleRef);

    std::vector<Constant *> functionRecords;
    for (size_t i = 0; i < recordsSize; ++i) {
        functionRecords.push_back(dyn_cast<Constant>(unwrap(records[i])));
    }
    llvm::SmallVector<StringRef, 16> filenameRefs;
    filenameRefs.resize(filenamesSize);
    for (size_t i = 0; i < filenamesSize; ++i) {
        if (sys::path::is_absolute(filenames[i])) {
            filenameRefs[filenamesIndices[i]] = filenames[i];
        } else {
            SmallString<256> path(filenames[i]);
            sys::fs::make_absolute(path);
            sys::path::remove_dots(path, true);
            filenameRefs[filenamesIndices[i]] = path;
        }
    }
    const std::string &rawCoverageMappings = createRawCoverageMapping(functionCoverages, functionCoveragesSize);
    GlobalVariable *coverageGlobal = emitCoverageGlobal(ctx, module, functionRecords, filenameRefs, rawCoverageMappings);

    const std::string &section = getInstrProfSectionName(IPSK_covmap, Triple(module.getTargetTriple()).getObjectFormat());
    coverageGlobal->setSection(section);
    coverageGlobal->setAlignment(8);
    return wrap(coverageGlobal);
}

LLVMValueRef LLVMInstrProfIncrement(LLVMModuleRef moduleRef) {
    Module &module = *unwrap(moduleRef);
    return wrap(Intrinsic::getDeclaration(&module, Intrinsic::instrprof_increment, None));
}

LLVMValueRef LLVMCreatePGOFunctionNameVar(LLVMValueRef llvmFunction, const char *pgoFunctionName) {
    auto *fnPtr = cast<llvm::Function>(unwrap(llvmFunction));
    return wrap(createPGOFuncNameVar(*fnPtr, pgoFunctionName));
}

void LLVMAddInstrProfPass(LLVMPassManagerRef passManagerRef, const char* outputFileName) {
    legacy::PassManagerBase *passManager = unwrap(passManagerRef);
    InstrProfOptions options;
    options.InstrProfileOutput = outputFileName;
    passManager->add(createInstrProfilingLegacyPass(options));
}

LLVMTargetLibraryInfoRef LLVMGetTargetLibraryInfo(LLVMModuleRef moduleRef) {
    auto* libraryInfo = new TargetLibraryInfoImpl(Triple(unwrap(moduleRef)->getTargetTriple()));
    return reinterpret_cast<LLVMTargetLibraryInfoRef>(libraryInfo);
}