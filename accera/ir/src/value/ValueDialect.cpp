////////////////////////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) Microsoft Corporation. All rights reserved.
//  Licensed under the MIT License. See LICENSE in the project root for license information.
//  Authors: Kern Handa
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "value/ValueDialect.h"

#include "value/ValueAttributes.h"
#include "value/ValueDialect.cpp.inc"
#include "value/ValueEnums.h"
#include "value/ValueFuncOp.h"

#include "IRUtil.h"

#include <mlir/Dialect/GPU/GPUDialect.h>
#include <mlir/Dialect/StandardOps/IR/Ops.h>
#include <mlir/IR/AffineMap.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/FunctionImplementation.h>
#include <mlir/Interfaces/CallInterfaces.h>

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/StringSwitch.h>
#include <llvm/ADT/TypeSwitch.h>

#include "value/ValueAttrs.cpp.inc"
#include "value/ValueOpsEnums.cpp.inc"

#include <numeric>

namespace accera::ir::value
{
void ValueDialect::initialize()
{
    addOperations<
        accera::ir::value::ValueFuncOp,
#define GET_OP_LIST
#include "value/ValueOps.cpp.inc"
        >();
    addTypes<RangeType>();
}

mlir::Type ValueDialect::parseType(mlir::DialectAsmParser& parser) const
{
    StringRef keyword;
    if (parser.parseKeyword(&keyword))
        return Type();

    MLIRContext* context = getContext();

    return llvm::StringSwitch<mlir::Type>(keyword)
        .Case("range", RangeType::get(context));

    parser.emitError(parser.getNameLoc(), "unknown value type: " + keyword);
    return Type();
}

void ValueDialect::printType(Type type, mlir::DialectAsmPrinter& os) const
{
    mlir::TypeSwitch<Type>(type)
        .Case<RangeType>([&](RangeType rangeTy) {
            // cf. mlir/lib/Dialect/Linalg/IR/LinalgTypes.cpp (llvm 13.0.1)
            os << "range";
        })
        .Default([](Type) { llvm_unreachable("unexpected 'value' type kind"); });
}

mlir::Operation* ValueDialect::materializeConstant(mlir::OpBuilder& builder, mlir::Attribute value, mlir::Type type, mlir::Location loc)
{
    return builder.create<mlir::arith::ConstantOp>(loc, value, type);
}
} // namespace accera::ir::value

using namespace llvm;
using namespace mlir;
using namespace accera::ir::value;

//===----------------------------------------------------------------------===//
// General helpers for comparison ops
//===----------------------------------------------------------------------===//

static void buildCmpOp(OpBuilder& build, OperationState& result, CmpOpPredicate predicate, Value lhs, Value rhs)
{
    result.addOperands({ lhs, rhs });
    auto boolType = build.getI1Type();
    if (auto vectorType = lhs.getType().dyn_cast<VectorType>())
    {
        auto shape = vectorType.getShape();
        auto resultType = mlir::VectorType::get(shape, boolType);

        result.types.push_back(resultType);
    }
    else
    {
        result.types.push_back(boolType);
    }
    result.addAttribute(
        CmpOp::getPredicateAttrName(),
        build.getI64IntegerAttr(static_cast<int64_t>(predicate)));
}

static void buildBinOp(OpBuilder& build, OperationState& result, BinaryOpPredicate predicate, Value lhs, Value rhs)
{
    result.addOperands({ lhs, rhs });
    result.types.push_back(lhs.getType());
    result.addAttribute(
        BinOp::getPredicateAttrName(),
        build.getI64IntegerAttr(static_cast<int64_t>(predicate)));
}

static void buildUnaryOp(OpBuilder& build, OperationState& result, UnaryOpPredicate predicate, Value input)
{
    result.addOperands({ input });
    result.types.push_back(input.getType().cast<ShapedType>().getElementType());
    result.addAttribute(
        UnaryOp::getPredicateAttrName(),
        build.getI64IntegerAttr(static_cast<int64_t>(predicate)));
}

void ValueFuncOp::build(OpBuilder& builder, OperationState& result, StringRef name, FunctionType type, ExecutionTarget target)
{
    result.addAttribute(mlir::SymbolTable::getSymbolAttrName(), builder.getStringAttr(name));
    result.addAttribute(getTypeAttrName(), TypeAttr::get(type));
    result.addAttribute(getExecTargetAttrName(), ExecutionTargetAttr::get(builder.getContext(), target));
    Region* body = result.addRegion();

    Block* entryBlock = new Block;
    entryBlock->addArguments(type.getInputs(), SmallVector<Location>(type.getInputs().size(), result.location));

    body->getBlocks().push_back(entryBlock);
}

void ValueFuncOp::build(OpBuilder& builder, OperationState& result, StringRef name, FunctionType type, ExecutionTarget target, ValueFuncOp::ExternalFuncTag)
{
    build(builder, result, name, type, target);
    result.addAttribute("external", builder.getUnitAttr());
    OpBuilder::InsertionGuard guard{ builder };
    builder.setInsertionPointToEnd(&result.regions[0]->front());
    builder.create<ir::value::ReturnOp>(result.location);
}

/// Hook for FunctionLike verifier.
LogicalResult ValueFuncOp::verifyType()
{
    Type type = getTypeAttr().getValue();
    if (!type.isa<FunctionType>())
    {
        return emitOpError("requires '" + getTypeAttrName() + "' attribute of function type");
    }

    return success();
}

// CallableOpInterface
Region* ValueFuncOp::getCallableRegion()
{
    return isExternal() ? nullptr : &getBody();
}

// CallableOpInterface
ArrayRef<Type> ValueFuncOp::getCallableResults()
{
    return getType().getResults();
}

ParseResult ValueFuncOp::parse(OpAsmParser& parser, OperationState& result)
{
    auto buildFuncType = [](Builder& builder, ArrayRef<Type> argTypes, ArrayRef<Type> results, function_interface_impl::VariadicFlag, std::string&) {
        return builder.getFunctionType(argTypes, results);
    };

    return function_interface_impl::parseFunctionOp(parser, result, /*allowVariadic=*/false, buildFuncType);
}

void ValueFuncOp::print(OpAsmPrinter& p)
{
    FunctionType fnType = getType();
    function_interface_impl::printFunctionOp(p, *this, fnType.getInputs(), /*isVariadic=*/false, fnType.getResults());
}

LogicalResult ValueFuncOp::verify()
{
    // If this function is external there is nothing to do.
    if (isExternal())
        return success();

    // Verify that the argument list of the function and the arg list of the entry
    // block line up.  The trait already verified that the number of arguments is
    // the same between the signature and the block.
    auto fnInputTypes = getType().getInputs();
    Block& entryBlock = front();
    for (unsigned i = 0, e = entryBlock.getNumArguments(); i != e; ++i)
        if (fnInputTypes[i] != entryBlock.getArgument(i).getType())
            return emitOpError("type of entry block argument #")
                   << i << '(' << entryBlock.getArgument(i).getType()
                   << ") must match the type of the corresponding argument in "
                   << "function signature(" << fnInputTypes[i] << ')';

    return success();
}

// cf: mlir/lib/IR/Function.cpp
void ValueFuncOp::eraseArguments(ArrayRef<unsigned> argIndices)
{
    auto oldType = getType();
    int originalNumArgs = oldType.getNumInputs();
    llvm::BitVector eraseIndices(originalNumArgs);
    for (auto index : argIndices)
    {
        eraseIndices.set(index);
    }
    auto shouldEraseArg = [&](int i) { return eraseIndices.test(i); };

    // There are 3 things that need to be updated:
    // - Function type.
    // - Arg attrs.
    // - Block arguments of entry block.

    // Update the function type and arg attrs.
    SmallVector<Type, 4> newInputTypes;
    SmallVector<DictionaryAttr, 4> newArgAttrs;
    for (int i = 0; i < originalNumArgs; i++)
    {
        if (shouldEraseArg(i))
        {
            continue;
        }
        newInputTypes.emplace_back(oldType.getInput(i));
        newArgAttrs.emplace_back(getArgAttrDict(i));
    }
    setType(FunctionType::get(getContext(), newInputTypes, oldType.getResults()));
    setAllArgAttrs(newArgAttrs);

    // Update the entry block's arguments.
    // We do this in reverse so that we erase later indices before earlier
    // indices, to avoid shifting the later indices.
    Block& entry = front();
    for (int i = 0; i < originalNumArgs; i++)
    {
        if (shouldEraseArg(originalNumArgs - i - 1))
        {
            entry.eraseArgument(originalNumArgs - i - 1);
        }
    }
}

void ValueLambdaOp::build(OpBuilder& builder, OperationState& result, StringRef name, FunctionType type, ExecutionTarget target)
{
    result.addAttribute(mlir::SymbolTable::getSymbolAttrName(), builder.getStringAttr(name));
    result.addAttribute(getTypeAttrName(), TypeAttr::get(type));
    result.addAttribute(getExecTargetAttrName(), ExecutionTargetAttr::get(builder.getContext(), target));

    Region* body = result.addRegion();
    Block* entryBlock = new Block;
    entryBlock->addArguments(type.getInputs(), SmallVector<Location>(type.getInputs().size(), result.location));

    body->getBlocks().push_back(entryBlock);
}

/// Hook for FunctionLike verifier.
LogicalResult ValueLambdaOp::verifyType()
{
    Type type = getTypeAttr().getValue();
    if (!type.isa<FunctionType>())
    {
        return emitOpError("requires '" + getTypeAttrName() + "' attribute of function type");
    }

    return success();
}

// CallableOpInterface
Region* ValueLambdaOp::getCallableRegion()
{
    return &body();
}

// CallableOpInterface
ArrayRef<Type> ValueLambdaOp::getCallableResults()
{
    return getType().getResults();
}

void ValueModuleOp::build(OpBuilder& builder, OperationState& result, StringRef name)
{
    Region* r = result.addRegion();
    ensureTerminator(*r, builder, result.location);
    result.attributes.push_back(builder.getNamedAttr(::mlir::SymbolTable::getSymbolAttrName(), builder.getStringAttr(name)));
}

void GlobalOp::build(
    OpBuilder& builder,
    OperationState& result,
    MemRefType type,
    bool isConstant,
    StringRef name,
    Attribute value,
    unsigned addrSpace,
    bool isExternal)
{
    result.addAttribute(SymbolTable::getSymbolAttrName(),
                        builder.getStringAttr(name));
    result.addAttribute("type", TypeAttr::get(type));
    if (isConstant)
        result.addAttribute("constant", builder.getUnitAttr());
    if (isExternal)
        result.addAttribute("external", builder.getUnitAttr());
    if (value)
        result.addAttribute("value", value);
    if (addrSpace != 0)
        result.addAttribute("addr_space", builder.getI32IntegerAttr(addrSpace));
}

static bool satisfiesModule(Operation* op)
{
    return op->hasTrait<OpTrait::SymbolTable>() &&
           op->hasTrait<OpTrait::IsIsolatedFromAbove>();
}

GlobalOp ReferenceGlobalOp::getGlobal()
{
    Operation* module = (*this)->getParentOp();
    while (module && !satisfiesModule(module))
        module = module->getParentOp();
    assert(module && "unexpected operation outside of a module");
    return dyn_cast_or_null<GlobalOp>(
        mlir::SymbolTable::lookupSymbolIn(module, global_name()));
}

FunctionType accera::ir::value::CallOp::getCalleeType()
{
    SmallVector<Type, 8> argTypes(getOperandTypes());
    return FunctionType::get(getContext(), argTypes, getResultTypes());
}

OpFoldResult GetElementOp::fold(ArrayRef<Attribute> operands)
{
    if (getOperand().getType() == getType())
        return getOperand();

    return {};
}

// Inspired by arith::IndexCastOp::fold()
OpFoldResult CastOp::fold(ArrayRef<Attribute> operands)
{
    // cast(constant int) -> constant int
    // A little hack because we go through int. Otherwise, the size of the
    // constant might need to change.
    if (auto value = operands[0].dyn_cast_or_null<mlir::IntegerAttr>())
    {
        auto castType = getType();
        if (castType.isSignlessIntOrIndex())
        {
            return IntegerAttr::get(castType, value.getInt());
        }
        else if (castType.isSignlessIntOrIndexOrFloat())
        {
            return FloatAttr::get(castType, value.getInt());
        }
    }

    return {};
}

void ReorderOp::build(OpBuilder& builder,
                      OperationState& result,
                      Value source,
                      ArrayAttr orderAttr)
{
    auto context = builder.getContext();

    // Compute the result memref type
    // Assume (for now) that source hasn't been permuted
    auto sourceType = source.getType().cast<MemRefType>();
    auto originalSizes = sourceType.getShape();

    // Compute permuted sizes and affine map permutation
    std::vector<int64_t> dimOrder = util::ConvertArrayAttrToIntVector(orderAttr);
    std::vector<int64_t> permutedSizes(dimOrder.size());
    std::vector<unsigned> affineMapOrder(dimOrder.size());

    for (auto en : llvm::enumerate(dimOrder))
    {
        permutedSizes[en.index()] = originalSizes[en.value()];
        affineMapOrder[en.value()] = en.index();
    }
    auto permutationMap = AffineMap::getPermutationMap(affineMapOrder, context);
    assert(permutationMap);

    // Compute permuted strides.
    int64_t offset;
    SmallVector<int64_t, 4> strides;
    [[maybe_unused]] auto res = getStridesAndOffset(sourceType, strides, offset);
    assert(succeeded(res));
    auto map = makeStridedLinearLayoutMap(strides, offset, context);
    map = map.compose(permutationMap);

    // Compute result type.
    MemRefType resultType = MemRefType::Builder(sourceType).setShape(permutedSizes).setLayout(AffineMapAttr::get(map));

    build(builder, result, resultType, source, orderAttr);
}

void ReduceOp::build(OpBuilder& builder, OperationState& result, Value input, Value initArg, BodyBuilderFn bodyBuilder)
{
    [[maybe_unused]] auto elementType = input.getType().cast<ShapedType>().getElementType();
    result.addOperands(input);
    result.addOperands(initArg);
    result.addTypes(initArg.getType());
    Region* bodyRegion = result.addRegion();
    bodyRegion->push_back(new Block);
    Block& bodyBlock = bodyRegion->front();
    bodyBlock.addArgument(initArg.getType(), result.location);
    bodyBlock.addArgument(initArg.getType(), result.location);

    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(&bodyBlock);
    if (bodyBuilder)
        bodyBuilder(builder, result.location, bodyBlock.getArgument(0), bodyBlock.getArgument(1));
}

void MapReduceOp::build(OpBuilder& builder, OperationState& result, Value input, Value initArg, MapBodyBuilderFn mapBodyBuilder, ReduceBodyBuilderFn reduceBodyBuilder)
{
    [[maybe_unused]] auto elementType = input.getType().cast<ShapedType>().getElementType();
    result.addOperands(input);
    result.addOperands(initArg);
    result.addTypes(initArg.getType());

    // Map body
    Region* mapBodyRegion = result.addRegion();
    mapBodyRegion->push_back(new Block);
    Block& mapBodyBlock = mapBodyRegion->front();
    mapBodyBlock.addArgument(initArg.getType(), result.location);
    {
        OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToStart(&mapBodyBlock);
        if (mapBodyBuilder)
            mapBodyBuilder(builder, result.location, mapBodyBlock.getArgument(0));
    }

    // Reduce body
    Region* reduceBodyRegion = result.addRegion();
    reduceBodyRegion->push_back(new Block);
    Block& reduceBodyBlock = reduceBodyRegion->front();
    reduceBodyBlock.addArgument(initArg.getType(), result.location);
    reduceBodyBlock.addArgument(initArg.getType(), result.location);
    {
        OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToStart(&reduceBodyBlock);
        if (reduceBodyBuilder)
            reduceBodyBuilder(builder, result.location, reduceBodyBlock.getArgument(0), reduceBodyBlock.getArgument(1));
    }
}

MMAOp::MMAOp(MMAShape shape_) :
    shape{ shape_ }
{
    switch (shape)
    {
    case MMAShape::M64xN64xK1_B4:
        m = 64;
        n = 64;
        k = 1;
        blocks = 4;
        break;
    case MMAShape::M64xN64xK1_B2:
        m = 64;
        n = 64;
        k = 1;
        blocks = 2;
        break;
    case MMAShape::M64xN64xK2_B4:
        m = 64;
        n = 64;
        k = 2;
        blocks = 4;
        break;
    case MMAShape::M64xN64xK2_B2:
        m = 64;
        n = 64;
        k = 2;
        blocks = 2;
        break;
    case MMAShape::M64xN64xK4_B4:
        m = 64;
        n = 64;
        k = 4;
        blocks = 4;
        break;
    case MMAShape::M64xN64xK4_B2:
        m = 64;
        n = 64;
        k = 4;
        blocks = 2;
        break;
    case MMAShape::M32xN32xK2_B1:
        m = 32;
        n = 32;
        k = 2;
        blocks = 1;
        break;
    case MMAShape::M32xN32xK4_B1:
        m = 32;
        n = 32;
        k = 4;
        blocks = 1;
        break;
    case MMAShape::M32xN32xK8_B1:
        m = 32;
        n = 32;
        k = 8;
        blocks = 1;
        break;
    case MMAShape::M16xN16xK4_B1:
        m = 16;
        n = 16;
        k = 4;
        blocks = 1;
        break;
    case MMAShape::M16xN16xK8_B1:
        m = 16;
        n = 16;
        k = 8;
        blocks = 1;
        break;
    case MMAShape::M16xN16xK16_B1:
        m = 16;
        n = 16;
        k = 16;
        blocks = 1;
        break;
    case MMAShape::M32xN8xK16_B1:
        m = 32;
        n = 8;
        k = 16;
        blocks = 1;
        break;
    case MMAShape::M8xN32xK16_B1:
        m = 8;
        n = 32;
        k = 16;
        blocks = 1;
        break;
    default:
        assert(false && "Invalid MMA shape.");
        break;
    }
}

MMAShape MMAOp::getShapeType() const
{
    return shape;
}

int64_t MMAOp::getInElementsPerThread(const int64_t warpSize) const
{
    return m * k / warpSize;
}

int64_t MMAOp::getOutElementsPerThread(const int64_t warpSize) const
{
    return getM() * getN() / warpSize;
}

int64_t MMAOp::getNumBlocks() const
{
    return blocks;
}

std::vector<int64_t> MMAOp::getOperandShape(const MMAOperandType operandType) const
{
    switch (operandType)
    {
    case MMAOperandType::A:
        return { getM(), getK() };
    case MMAOperandType::B:
        return { getK(), getN() };
    case MMAOperandType::Acc:
        return { getM(), getN() };
    default:
        return {};
    }
}

std::vector<uint8_t> MMAOp::getOffsetMap() const
{
    // These index offsets are calculated based on the data layout in which
    // AMD mfma operation maps them to different threads.
    if (getNumBlocks() == 2 || m == 32) // M64xN64xK1_B2, M64xN64xK4_B2, M32xN32xK2_B1, M32xN32xK8_B1
        return { 0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15, 16, 20, 17, 21, 18, 22, 19, 23, 24, 28, 25, 29, 26, 30, 27, 31 };

    return { 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15 }; // M64xN64xK1_B4, M64xN64xK4_B4, M16xN16xK4_B1, M16xN16xK16_B1
}

std::array<int64_t, 2> MMAOp::getOffsetMapSize() const
{
    // The offset map is organised in this layout so that it can be indexed by thread id.
    if (getNumBlocks() == 2 || m == 32) // M64xN64xK1_B2, M64xN64xK4_B2, M32xN32xK2_B1, M32xN32xK8_B1
        return { 16, 2 };

    return { 4, 4 }; // M64xN64xK1_B4, M64xN64xK4_B4, M16xN16xK4_B1, M16xN16xK16_B1
}

std::pair<mlir::MemRefType, mlir::RankedTensorType> MMAOp::GetMFMAThreadOffsetMapType(IntegerType mlirElemType) const
{
    auto vecSize = getOffsetMapSize();
    return std::make_pair(mlir::MemRefType::get(vecSize, mlirElemType, {}, mlir::gpu::GPUDialect::getPrivateAddressSpace()), mlir::RankedTensorType::get(vecSize, mlirElemType));
}

//===----------------------------------------------------------------------===//
// MMA Ops
//===----------------------------------------------------------------------===//

static LogicalResult verify(MMAComputeSyncOp op)
{
    auto opAType = op.opA().getType().cast<MemRefType>().getElementType();
    auto opBType = op.opB().getType().cast<MemRefType>().getElementType();
    if (opAType != opBType)
        return op.emitError("Invalid data types for A and B.");

    return success();
}

static LogicalResult verify(MMAFillSyncOp op)
{
    auto value = op.value();
    auto valueType = value.getType();

    if (valueType != op.dest().getType().cast<MemRefType>().getElementType())
        return op.emitError("value type must match matrix element type");

    return success();
}

static LogicalResult verify(MMALoadSyncOp op)
{
    auto srcType = op.getMemRefType();
    MMAOperandType operand{ op.operandType() };
    const MemorySpace srcMemSpace{ srcType.getMemorySpaceAsInt() };

    if (srcMemSpace != MemorySpace::None && srcMemSpace != MemorySpace::Shared &&
        srcMemSpace != MemorySpace::Global && srcMemSpace != MemorySpace::Private && srcMemSpace != MemorySpace::Tensor)
        return op.emitError(
            "source memorySpace None, Shared, Private, Global or Tensor only allowed");

    if (operand != MMAOperandType::A && operand != MMAOperandType::B &&
        operand != MMAOperandType::Acc)
        return op.emitError("only AOp, BOp and COp can be loaded");

    return success();
}

static LogicalResult verify(MMAStoreSyncOp op)
{
    auto dstMemrefType = op.getMemRefType();
    const MemorySpace dstMemSpace{ dstMemrefType.getMemorySpaceAsInt() };

    if (dstMemSpace != MemorySpace::None && dstMemSpace != MemorySpace::Shared &&
        dstMemSpace != MemorySpace::Global && dstMemSpace != MemorySpace::Private && dstMemSpace != MemorySpace::Tensor)
        return op.emitError(
            "destination memorySpace of None, Global, Shared, Private or Tensor only allowed");

    return success();
}

static LogicalResult verify(GPUBlockCacheOp op)
{
    auto tileShape = op.tileShape();
    if (tileShape.size() != 2)
    {
        return op.emitError("Only 2-D tiles are supported.");
    }

    auto dstMemrefType = op.dest().getType().cast<MemRefType>();
    const MemorySpace destMemSpace{ dstMemrefType.getMemorySpaceAsInt() };
    const auto dstShape = dstMemrefType.getShape();
    if (dstShape.size() != 2)
    {
        return op.emitError("Only 2-D destination memrefs are supported.");
    }

    if (op.workPerThread() < 1 || op.vecWidth() < 1 || op.workPerThread() % op.vecWidth() != 0)
    {
        return op.emitError("Work per thread (WPT) must be >= 1 and vector width must be >= 1 and WPT must be a multiple of vector width.");
    }

    const auto tileShapeVec = accera::ir::util::ConvertArrayAttrToIntVector(tileShape);
    assert(!(destMemSpace == MemorySpace::Shared && op.dstRowMajor()) || (dstShape[0] == tileShapeVec[0] && dstShape[1] == tileShapeVec[1]));
    assert(!(destMemSpace == MemorySpace::Shared && !op.dstRowMajor()) || (dstShape[0] == tileShapeVec[1] && dstShape[1] == tileShapeVec[0]));

    return success();
}

// TableGen'd op method definitions
#define GET_OP_CLASSES
#include "value/ValueOps.cpp.inc"
