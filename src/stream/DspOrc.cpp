#include "stream/DspProgramData.hpp"

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <limits>
#include <numbers>
#include <string>
#include <unordered_map>

namespace openstone::stream::detail {
namespace {

constexpr unsigned laneCount = std::tuple_size_v<DspLanes>;
static_assert(sizeof(DspLanes) == laneCount * sizeof(std::uint64_t));

struct Register {
    DspType type;
    llvm::Value* bits; // <4 x i64>, matching DSP slots in host memory.
};

struct LocalSubroutine {
    std::vector<std::size_t> continuations;
    llvm::AllocaInst* savedContinuation{};
};

class StageLowerer {
public:
    StageLowerer(llvm::Module& module, llvm::StringRef name, std::span<const DspInstruction> code)
        : module_(module), ir_(module.getContext()), code_(code),
          leaders_(code.size() + 1), loops_(code.size())
    {
        auto* pointer = ir_.getPtrTy();
        auto* signature = llvm::FunctionType::get(ir_.getVoidTy(), {pointer, pointer, pointer, ir_.getInt64Ty()}, false);
        function_ = llvm::Function::Create(signature, llvm::Function::ExternalLinkage, name, module_);
        function_->addFnAttr(llvm::Attribute::NoUnwind);
        slots_ = function_->getArg(0);
        arrays_ = function_->getArg(1);
        references_ = function_->getArg(2);
        frame_ = function_->getArg(3);
        slots_->setName("slots");
        arrays_->setName("arrays");
        references_->setName("references");
        frame_->setName("frame");

        ir_.SetInsertPoint(block("entry"));
        for (std::size_t pc = 0; pc < code_.size(); ++pc) {
            const auto& instruction = code_[pc];
            switch (instruction.op) {
            case DspOp::LocalCall:
                subroutines_[instruction.slot].continuations.push_back(pc + 1);
                markLeader(instruction.slot);
                [[fallthrough]];
            case DspOp::LocalReturn:
                markLeader(pc + 1);
                break;
            case DspOp::Loop:
                loops_[pc] = ir_.CreateAlloca(ir_.getInt64Ty(), nullptr, "loop.counter");
                [[fallthrough]];
            case DspOp::Repeat:
            case DspOp::Hop:
            case DspOp::JumpIfTrue:
            case DspOp::Jump:
                markLeader(pc + 1);
                markLeader(instruction.slot);
                if ((instruction.op == DspOp::JumpIfTrue || instruction.op == DspOp::Jump) &&
                    instruction.slot <= pc && !backwardBranches_) {
                    backwardBranches_ = ir_.CreateAlloca(ir_.getInt32Ty(), nullptr, "backward.branches");
                    ir_.CreateStore(ir_.getInt32(0), backwardBranches_);
                    markLeader(code_.size());
                }
                break;
            default:
                break;
            }
        }
        if (!subroutines_.empty()) {
            recursiveCalls_ = ir_.CreateAlloca(ir_.getInt32Ty(), nullptr, "recursive.calls");
            ir_.CreateStore(ir_.getInt32(0), recursiveCalls_);
            markLeader(code_.size());
        }
        for (auto& [entry, subroutine] : subroutines_) {
            if (subroutine.continuations.size() > 1) {
                // Each shared body is specialized to one depth and cannot be active twice.
                subroutine.savedContinuation = ir_.CreateAlloca(ir_.getInt64Ty(), nullptr, "local.continuation");
            }
        }
    }

    void lower()
    {
        for (std::size_t pc = 0; pc < code_.size(); ++pc) {
            enterLeader(pc);
            lower(code_[pc], pc);
        }
        enterLeader(code_.size());
        ir_.CreateRetVoid();
    }

private:
    llvm::BasicBlock* block(llvm::StringRef name)
    {
        return llvm::BasicBlock::Create(module_.getContext(), name, function_);
    }

    void markLeader(std::size_t pc)
    {
        if (!leaders_[pc]) {
            leaders_[pc] = block("section");
        }
    }

    void enterLeader(std::size_t pc)
    {
        if (auto* next = leaders_[pc]) {
            if (!ir_.GetInsertBlock()->getTerminator()) {
                ir_.CreateBr(next);
            }
            ir_.SetInsertPoint(next);
        }
    }

    llvm::FixedVectorType* lanes(llvm::Type* scalar)
    {
        return llvm::FixedVectorType::get(scalar, laneCount);
    }

    llvm::FixedVectorType* bitsType() { return lanes(ir_.getInt64Ty()); }

    llvm::Constant* splat(llvm::Constant* value)
    {
        return llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(laneCount), value);
    }

    llvm::Constant* integers(llvm::IntegerType* type, std::uint64_t value)
    {
        return splat(llvm::ConstantInt::get(type, value));
    }

    llvm::Constant* numbers(llvm::Type* type, double value)
    {
        return splat(llvm::ConstantFP::get(type, value));
    }

    llvm::Value* extract(llvm::Value* values, unsigned lane)
    {
        return ir_.CreateExtractElement(values, ir_.getInt32(lane));
    }

    llvm::Value* insert(llvm::Value* values, llvm::Value* value, unsigned lane)
    {
        return ir_.CreateInsertElement(values, value, ir_.getInt32(lane));
    }

    llvm::Value* slot(std::size_t offset)
    {
        return ir_.CreateGEP(bitsType(), slots_, ir_.getInt64(offset));
    }

    llvm::Value* load(llvm::Value* address)
    {
        return ir_.CreateAlignedLoad(bitsType(), address, llvm::Align(alignof(DspLanes)));
    }

    llvm::Value* raw32(llvm::Value* bits) { return ir_.CreateTrunc(bits, lanes(ir_.getInt32Ty())); }
    llvm::Value* wide(llvm::Value* bits) { return ir_.CreateZExt(bits, bitsType()); }
    llvm::Value* masks(llvm::Value* truth) { return ir_.CreateSExt(truth, bitsType()); }

    llvm::Value* floatValue(Register value)
    {
        if (value.type == DspType::Float) {
            return ir_.CreateBitCast(raw32(value.bits), lanes(ir_.getFloatTy()));
        }
        return ir_.CreateBitCast(value.bits, lanes(ir_.getDoubleTy()));
    }

    llvm::Value* floatingBits(llvm::Value* value)
    {
        if (value->getType()->getScalarType()->isFloatTy()) {
            return wide(ir_.CreateBitCast(value, lanes(ir_.getInt32Ty())));
        }
        return ir_.CreateBitCast(value, bitsType());
    }

    llvm::Value* asDouble(Register value)
    {
        auto* doubleLanes = lanes(ir_.getDoubleTy());
        switch (value.type) {
        case DspType::Float: return ir_.CreateFPExt(floatValue(value), doubleLanes);
        case DspType::Double: case DspType::Decimal: case DspType::Fraction: return floatValue(value);
        case DspType::Int: return ir_.CreateSIToFP(raw32(value.bits), doubleLanes);
        case DspType::Bool:
            return ir_.CreateUIToFP(ir_.CreateICmpNE(value.bits, integers(ir_.getInt64Ty(), 0)), doubleLanes);
        case DspType::Hex: return ir_.CreateUIToFP(value.bits, doubleLanes);
        }
        std::abort();
    }

    llvm::Value* truth(Register value)
    {
        if (value.type == DspType::Bool || value.type == DspType::Int || value.type == DspType::Hex) {
            return ir_.CreateICmpNE(value.bits, integers(ir_.getInt64Ty(), 0));
        }
        auto* floating = floatValue(value);
        return ir_.CreateFCmpUNE(floating, numbers(floating->getType()->getScalarType(), 0));
    }

    llvm::Value* asInt(llvm::Value* values)
    {
        auto* scalar = values->getType()->getScalarType();
        // Truncation permits fractional values just above INT_MAX or below INT_MIN.
        auto* valid = ir_.CreateAnd(ir_.CreateFCmpOGT(values, numbers(scalar, -2147483649.0)),
                                     ir_.CreateFCmpOLT(values, numbers(scalar, 2147483648.0)));
        auto* safe = ir_.CreateSelect(valid, values, numbers(scalar, 0));
        auto* converted = ir_.CreateFPToSI(safe, lanes(ir_.getInt32Ty()));
        return wide(ir_.CreateSelect(valid, converted, integers(ir_.getInt32Ty(), 0x80000000U)));
    }

    llvm::Value* convert(Register value, DspType type)
    {
        if (value.type == type) {
            return value.bits;
        }
        if (type == DspType::Bool) {
            return masks(truth(value));
        }
        if (value.type == DspType::Hex) {
            return type == DspType::Double ? value.bits : wide(raw32(value.bits));
        }
        auto* number = asDouble(value);
        switch (type) {
        case DspType::Int: return asInt(number);
        case DspType::Float: return floatingBits(ir_.CreateFPTrunc(number, lanes(ir_.getFloatTy())));
        case DspType::Double: return floatingBits(number);
        default: std::abort();
        }
    }

    llvm::Value* laneMask(unsigned mask)
    {
        std::array<llvm::Constant*, laneCount> lanes{};
        for (unsigned lane = 0; lane < laneCount; ++lane) {
            lanes[lane] = ir_.getInt1((mask & (1U << lane)) != 0);
        }
        return llvm::ConstantVector::get(lanes);
    }

    llvm::Value* broadcast(llvm::Value* values, int source)
    {
        if (source < 0) {
            return values;
        }
        std::array<int, laneCount> mask;
        mask.fill(source);
        return ir_.CreateShuffleVector(values, mask);
    }

    llvm::Value* selectLanes(llvm::Value* bits, DspSelection selection)
    {
        bits = broadcast(bits, selection.broadcast);
        if (selection.mask != 0xf) {
            bits = ir_.CreateSelect(laneMask(selection.mask), bits, integers(ir_.getInt64Ty(), 0));
        }
        return bits;
    }

    struct IndexedLane {
        llvm::Value* address;
        llvm::Value* valid;
    };

    IndexedLane indexedLane(llvm::Value* base, llvm::Value* index, const DspInstruction& op, unsigned lane)
    {
        auto* number = extract(index, lane);
        auto* valid = ir_.CreateAnd(ir_.CreateFCmpOGE(number, llvm::ConstantFP::get(ir_.getDoubleTy(), 0)),
            ir_.CreateFCmpOLT(number, llvm::ConstantFP::get(ir_.getDoubleTy(), static_cast<double>(op.count))));
        auto* safe = ir_.CreateSelect(valid, number, llvm::ConstantFP::get(ir_.getDoubleTy(), 0));
        auto* offset = ir_.CreateFPToUI(safe, ir_.getInt64Ty());
        auto* element = ir_.CreateAdd(ir_.CreateMul(offset, ir_.getInt64(laneCount)),
            ir_.getInt64(op.slot * laneCount + lane));
        return {ir_.CreateGEP(ir_.getInt64Ty(), base, element), valid};
    }

    llvm::Value* loadIndexed(llvm::Value* base, Register index, const DspInstruction& op)
    {
        auto* positions = asDouble(index);
        llvm::Value* values = llvm::PoisonValue::get(bitsType());
        for (unsigned lane = 0; lane < laneCount; ++lane) {
            const auto address = indexedLane(base, positions, op, lane);
            auto* raw = ir_.CreateLoad(ir_.getInt64Ty(), address.address);
            values = insert(values, ir_.CreateSelect(address.valid, raw, ir_.getInt64(0)), lane);
        }
        return selectLanes(values, op.lanes);
    }

    void storeIfValid(const IndexedLane& address, llvm::Value* value)
    {
        auto* write = block("memory.write");
        auto* next = block("memory.next");
        ir_.CreateCondBr(address.valid, write, next);
        ir_.SetInsertPoint(write);
        ir_.CreateStore(value, address.address);
        ir_.CreateBr(next);
        ir_.SetInsertPoint(next);
    }

    void storeIndexed(Register index, Register value, const DspInstruction& op)
    {
        auto* values = broadcast(convert(value, op.type), op.lanes.broadcast);
        auto* positions = asDouble(index);
        for (unsigned lane = 0; lane < laneCount; ++lane) {
            if ((op.lanes.mask & (1U << lane)) == 0) {
                continue;
            }
            const auto address = indexedLane(slots_, positions, op, lane);
            storeIfValid(address, extract(values, lane));
        }
    }

    IndexedLane assemblyMemoryLane(Register index, const DspInstruction& op, unsigned lane)
    {
        const auto storage = static_cast<AssemblyStorage>(op.bits);
        auto* base = storage == AssemblyStorage::ArrayInput ? arrays_ : slots_;
        const auto stride = storage == AssemblyStorage::Packet ? 1 : laneCount;
        const auto bytes = assemblyElementBytes(op.type);
        auto* offset = extract(raw32(index.bits), 0);
        auto* aligned = ir_.CreateICmpEQ(ir_.CreateAnd(offset, ir_.getInt32(bytes - 1)), ir_.getInt32(0));
        auto* nonnegative = ir_.CreateICmpSGE(offset, ir_.getInt32(0));
        auto* first = ir_.CreateZExt(ir_.CreateUDiv(offset, ir_.getInt32(bytes)), ir_.getInt64Ty());
        auto* element = ir_.CreateAdd(first, ir_.getInt64(lane));
        auto* valid = ir_.CreateAnd(ir_.CreateAnd(aligned, nonnegative), ir_.CreateICmpULT(element, ir_.getInt64(op.count)));
        auto* safe = ir_.CreateSelect(valid, element, ir_.getInt64(0));
        auto* position = ir_.CreateAdd(ir_.CreateMul(safe, ir_.getInt64(stride)), ir_.getInt64(op.slot * laneCount));
        return {ir_.CreateGEP(ir_.getInt64Ty(), base, position), valid};
    }

    llvm::Value* loadAssemblyMemory(Register index, const DspInstruction& op)
    {
        llvm::Value* bits = integers(ir_.getInt64Ty(), 0);
        const auto mask = op.lanes.broadcast < 0 ? op.lanes.mask : 1U << op.lanes.broadcast;
        for (unsigned lane = 0; lane < assemblyPacketLanes(op.type); ++lane) {
            if ((mask & (1U << lane)) == 0) continue;
            const auto address = assemblyMemoryLane(index, op, lane);
            auto* raw = ir_.CreateLoad(ir_.getInt64Ty(), address.address);
            bits = insert(bits, ir_.CreateSelect(address.valid, raw, ir_.getInt64(0)), lane);
        }
        return selectLanes(bits, op.lanes);
    }

    void storeAssemblyMemory(Register index, Register value, const DspInstruction& op)
    {
        auto* bits = convert(value, op.type);
        for (unsigned lane = 0; lane < assemblyPacketLanes(op.type); ++lane) {
            if ((op.lanes.mask & (1U << lane)) == 0) continue;
            storeIfValid(assemblyMemoryLane(index, op, lane), extract(bits, lane));
        }
    }

    Register reinterpret128(Register value, DspType type)
    {
        auto* zero = integers(ir_.getInt64Ty(), 0);
        llvm::Value* result = zero;
        if (type == DspType::Double) {
            for (unsigned lane = 0; lane < 2; ++lane) {
                auto* bits = value.type == DspType::Double ? extract(value.bits, lane)
                    : ir_.CreateOr(extract(wide(raw32(value.bits)), lane * 2),
                                   ir_.CreateShl(extract(wide(raw32(value.bits)), lane * 2 + 1), ir_.getInt64(32)));
                result = insert(result, bits, lane);
            }
        } else if (value.type == DspType::Double) {
            for (unsigned lane = 0; lane < 4; ++lane) {
                auto* bits = ir_.CreateLShr(extract(value.bits, lane / 2), ir_.getInt64((lane % 2) * 32));
                result = insert(result, ir_.CreateAnd(bits, ir_.getInt64(UINT32_MAX)), lane);
            }
        } else {
            result = wide(raw32(value.bits));
        }
        return {type, result};
    }

    struct PacketWord {
        IndexedLane cell;
        llvm::Value* shift;
    };

    PacketWord assemblyPacketWord(Register index, const DspInstruction& op, unsigned lane, unsigned word)
    {
        const auto storageBytes = assemblyElementBytes(op.storageType);
        const auto bytes = assemblyElementBytes(op.type);
        auto* offset = ir_.CreateSExt(extract(raw32(index.bits), 0), ir_.getInt64Ty());
        auto* first = ir_.CreateAdd(offset, ir_.getInt64(lane * bytes));
        auto* aligned = ir_.CreateICmpEQ(ir_.CreateAnd(offset, ir_.getInt64(3)), ir_.getInt64(0));
        auto* valid = ir_.CreateAnd(ir_.CreateAnd(aligned, ir_.CreateICmpSGE(offset, ir_.getInt64(0))),
            ir_.CreateICmpULE(ir_.CreateAdd(first, ir_.getInt64(bytes)), ir_.getInt64(op.count * storageBytes)));
        auto* position = ir_.CreateAdd(first, ir_.getInt64(word * 4));
        auto* element = ir_.CreateUDiv(position, ir_.getInt64(storageBytes));
        auto* safe = ir_.CreateSelect(valid, element, ir_.getInt64(0));
        const auto storage = static_cast<AssemblyStorage>(op.bits);
        auto* base = storage == AssemblyStorage::ArrayInput ? arrays_ : slots_;
        const auto stride = storage == AssemblyStorage::Packet ? 1 : laneCount;
        auto* address = ir_.CreateAdd(ir_.CreateMul(safe, ir_.getInt64(stride)), ir_.getInt64(op.slot * laneCount));
        auto* shift = ir_.CreateMul(ir_.CreateURem(position, ir_.getInt64(storageBytes)), ir_.getInt64(8));
        return {{ir_.CreateGEP(ir_.getInt64Ty(), base, address), valid}, shift};
    }

    llvm::Value* loadAssemblyPacket(Register index, const DspInstruction& op)
    {
        llvm::Value* result = integers(ir_.getInt64Ty(), 0);
        const auto bytes = assemblyElementBytes(op.type);
        for (unsigned lane = 0; lane < assemblyPacketLanes(op.type); ++lane) {
            llvm::Value* bits = ir_.getInt64(0);
            for (unsigned word = 0; word < bytes / 4; ++word) {
                const auto address = assemblyPacketWord(index, op, lane, word);
                auto* raw = ir_.CreateLShr(ir_.CreateLoad(ir_.getInt64Ty(), address.cell.address), address.shift);
                auto* piece = ir_.CreateAnd(raw, ir_.getInt64(UINT32_MAX));
                piece = ir_.CreateSelect(address.cell.valid, piece, ir_.getInt64(0));
                bits = ir_.CreateOr(bits, ir_.CreateShl(piece, ir_.getInt64(word * 32)));
            }
            result = insert(result, bits, lane);
        }
        return selectLanes(result, op.lanes);
    }

    void storeAssemblyPacket(Register index, Register value, const DspInstruction& op)
    {
        const auto bytes = assemblyElementBytes(op.type);
        for (unsigned lane = 0; lane < assemblyPacketLanes(op.type); ++lane) {
            if ((op.lanes.mask & (1U << lane)) == 0) continue;
            const auto source = op.lanes.broadcast < 0 ? lane : static_cast<unsigned>(op.lanes.broadcast);
            for (unsigned word = 0; word < bytes / 4; ++word) {
                const auto address = assemblyPacketWord(index, op, lane, word);
                auto* piece = ir_.CreateAnd(ir_.CreateLShr(extract(value.bits, source), ir_.getInt64(word * 32)), ir_.getInt64(UINT32_MAX));
                llvm::Value* bits = piece;
                if (op.storageType == DspType::Double) {
                    auto* before = ir_.CreateLoad(ir_.getInt64Ty(), address.cell.address);
                    auto* mask = ir_.CreateShl(ir_.getInt64(UINT32_MAX), address.shift);
                    bits = ir_.CreateOr(ir_.CreateAnd(before, ir_.CreateNot(mask)), ir_.CreateShl(piece, address.shift));
                }
                storeIfValid(address.cell, bits);
            }
        }
    }

    PacketWord assemblyByte(Register index, const DspInstruction& op)
    {
        const auto bytes = assemblyElementBytes(op.storageType);
        auto* offset = ir_.CreateSExt(extract(raw32(index.bits), 0), ir_.getInt64Ty());
        auto* valid = ir_.CreateAnd(ir_.CreateICmpSGE(offset, ir_.getInt64(0)),
                                  ir_.CreateICmpULT(offset, ir_.getInt64(op.count * bytes)));
        auto* element = ir_.CreateSelect(valid, ir_.CreateUDiv(offset, ir_.getInt64(bytes)), ir_.getInt64(0));
        const auto storage = static_cast<AssemblyStorage>(op.bits);
        auto* base = storage == AssemblyStorage::ArrayInput ? arrays_ : slots_;
        auto* position = ir_.CreateAdd(ir_.CreateMul(element, ir_.getInt64(storage == AssemblyStorage::Packet ? 1 : laneCount)),
                                      ir_.getInt64(op.slot * laneCount));
        auto* shift = ir_.CreateMul(ir_.CreateAnd(offset, ir_.getInt64(bytes - 1)), ir_.getInt64(8));
        return {{ir_.CreateGEP(ir_.getInt64Ty(), base, position), valid}, shift};
    }

    llvm::Value* loadAssemblyByte(Register index, const DspInstruction& op)
    {
        const auto address = assemblyByte(index, op);
        auto* raw = ir_.CreateLoad(ir_.getInt64Ty(), address.cell.address);
        auto* value = ir_.CreateAnd(ir_.CreateLShr(raw, address.shift), ir_.getInt64(0xff));
        return ir_.CreateVectorSplat(laneCount, ir_.CreateSelect(address.cell.valid, value, ir_.getInt64(0)));
    }

    void storeAssemblyByte(Register index, Register value, const DspInstruction& op)
    {
        const auto address = assemblyByte(index, op);
        auto* before = ir_.CreateLoad(ir_.getInt64Ty(), address.cell.address);
        auto* mask = ir_.CreateShl(ir_.getInt64(0xff), address.shift);
        auto* byte = ir_.CreateAnd(extract(value.bits, 0), ir_.getInt64(0xff));
        auto* bits = ir_.CreateOr(ir_.CreateAnd(before, ir_.CreateNot(mask)), ir_.CreateShl(byte, address.shift));
        storeIfValid(address.cell, bits);
    }

    llvm::Value* engineBase(const DspInstruction& op)
    {
        return op.count ? ir_.CreateSExt(extract(raw32(load(slot(op.count - 1))), 0), ir_.getInt64Ty()) : ir_.getInt64(engineRootAddress);
    }

    llvm::Value* engineOffset(const DspInstruction& op, llvm::Value* position, unsigned bytes)
    {
        if (op.slot < enginePointerPortBase || op.slot == nativeFramePort) return position;
        auto* signature = llvm::FunctionType::get(ir_.getInt64Ty(), {ir_.getPtrTy(), ir_.getInt64Ty(), ir_.getInt64Ty(), ir_.getInt64Ty(), ir_.getInt32Ty()}, false);
        return ir_.CreateCall(module_.getOrInsertFunction("openstone_dsp_engine_offset", signature), {references_, ir_.getInt64(op.slot), engineBase(op), position, ir_.getInt32(bytes)});
    }

    llvm::Value* loadMemRef(Register index, const DspInstruction& op)
    {
        auto* signature = llvm::FunctionType::get(ir_.getInt64Ty(),
            {ir_.getPtrTy(), ir_.getInt64Ty(), ir_.getInt64Ty(), ir_.getInt32Ty()}, false);
        const auto function = module_.getOrInsertFunction(op.op == DspOp::LoadMemArrayField
            ? "openstone_dsp_read_memarray_field" : "openstone_dsp_read_memref", signature);
        const auto bytes = op.op == DspOp::LoadMemByte ? 1U : assemblyElementBytes(op.type);
        auto* offset = ir_.CreateSExt(extract(raw32(index.bits), 0), ir_.getInt64Ty());
        llvm::Value* bits = integers(ir_.getInt64Ty(), 0);
        const auto mask = op.lanes.broadcast < 0 ? op.lanes.mask : 1U << op.lanes.broadcast;
        for (unsigned lane = 0; lane < assemblyPacketLanes(op.type); ++lane) {
            if ((mask & (1U << lane)) == 0) continue;
            auto* position = ir_.CreateAdd(offset, ir_.getInt64(lane * bytes));
            auto* value = ir_.CreateCall(function, {references_, ir_.getInt64(op.slot), engineOffset(op, position, bytes), ir_.getInt32(bytes)});
            bits = insert(bits, value, lane);
        }
        return selectLanes(bits, op.lanes);
    }

    void storeMemRef(Register index, Register value, const DspInstruction& op)
    {
        auto* signature = llvm::FunctionType::get(ir_.getVoidTy(),
            {ir_.getPtrTy(), ir_.getInt64Ty(), ir_.getInt64Ty(), ir_.getInt32Ty(), ir_.getInt64Ty(), ir_.getInt32Ty()}, false);
        const auto function = module_.getOrInsertFunction("openstone_dsp_write_memref", signature);
        const auto bytes = op.op == DspOp::StoreMemByte ? 1U : assemblyElementBytes(op.type);
        auto* offset = ir_.CreateSExt(extract(raw32(index.bits), 0), ir_.getInt64Ty());
        auto* bits = convert(value, op.type);
        for (unsigned lane = 0; lane < assemblyPacketLanes(op.type); ++lane) {
            if ((op.lanes.mask & (1U << lane)) == 0) continue;
            auto* position = ir_.CreateAdd(offset, ir_.getInt64(lane * bytes));
            ir_.CreateCall(function, {references_, ir_.getInt64(op.slot), engineOffset(op, position, bytes), ir_.getInt32(bytes), extract(bits, lane), ir_.getInt32(op.bits)});
        }
    }

    llvm::Value* libm(llvm::StringRef name, llvm::Value* left, llvm::Value* right = nullptr)
    {
        auto* scalar = left->getType()->getScalarType();
        llvm::SmallVector<llvm::Type*, 2> parameters{scalar};
        if (right) {
            parameters.push_back(scalar);
        }
        const auto symbol = scalar->isFloatTy() ? (name + "f").str() : name.str();
        auto callee = module_.getOrInsertFunction(symbol, llvm::FunctionType::get(scalar, parameters, false));
        llvm::Value* result = llvm::PoisonValue::get(left->getType());
        for (unsigned lane = 0; lane < laneCount; ++lane) {
            llvm::SmallVector<llvm::Value*, 2> args{extract(left, lane)};
            if (right) {
                args.push_back(extract(right, lane));
            }
            result = insert(result, ir_.CreateCall(callee, args), lane);
        }
        return result;
    }

    llvm::Value* comparison(DspOp op, llvm::Value* a, llvm::Value* b)
    {
        switch (op) {
        case DspOp::Less: return ir_.CreateFCmpOLT(a, b);
        case DspOp::LessEqual: return ir_.CreateFCmpOLE(a, b);
        case DspOp::Greater: return ir_.CreateFCmpOGT(a, b);
        case DspOp::GreaterEqual: return ir_.CreateFCmpOGE(a, b);
        case DspOp::Equal: return ir_.CreateFCmpOEQ(a, b);
        case DspOp::NotEqual: return ir_.CreateFCmpUNE(a, b);
        default: std::abort();
        }
    }

    llvm::Value* integerBinary(DspOp op, llvm::Value* a, llvm::Value* b)
    {
        switch (op) {
        case DspOp::Add: return ir_.CreateAdd(a, b);
        case DspOp::Subtract: return ir_.CreateSub(a, b);
        case DspOp::Multiply: return ir_.CreateMul(a, b);
        case DspOp::Min: return ir_.CreateSelect(ir_.CreateICmpSLT(a, b), a, b);
        case DspOp::Max: return ir_.CreateSelect(ir_.CreateICmpSGT(a, b), a, b);
        case DspOp::Divide: case DspOp::Remainder: {
            auto* zero = integers(ir_.getInt32Ty(), 0);
            auto* valid = ir_.CreateICmpNE(b, zero);
            auto* numerator = ir_.CreateSExt(a, bitsType());
            auto* divisor = ir_.CreateSExt(ir_.CreateSelect(valid, b, integers(ir_.getInt32Ty(), 1)), bitsType());
            auto* result = op == DspOp::Divide ? ir_.CreateSDiv(numerator, divisor) : ir_.CreateSRem(numerator, divisor);
            return ir_.CreateSelect(valid, raw32(result), zero);
        }
        default: std::abort();
        }
    }

    llvm::Value* floatingBinary(DspOp op, llvm::Value* a, llvm::Value* b)
    {
        switch (op) {
        case DspOp::Add: return ir_.CreateFAdd(a, b);
        case DspOp::Subtract: return ir_.CreateFSub(a, b);
        case DspOp::Multiply: return ir_.CreateFMul(a, b);
        case DspOp::Divide: return ir_.CreateFDiv(a, b);
        case DspOp::Remainder: return libm("fmod", a, b);
        case DspOp::Power: return libm("pow", a, b);
        case DspOp::Min: return libm("fmin", a, b);
        case DspOp::Max: return libm("fmax", a, b);
        case DspOp::ScalePowerOfTwo: {
            auto* scalar = b->getType()->getScalarType();
            auto* infinity = numbers(scalar, std::numeric_limits<double>::infinity());
            auto* negativeInfinity = ir_.CreateFNeg(infinity);
            auto* finite = ir_.CreateAnd(ir_.CreateFCmpOLT(b, infinity), ir_.CreateFCmpOGT(b, negativeInfinity));
            auto* zero = numbers(scalar, 0);
            auto* safe = ir_.CreateSelect(finite, b, zero);
            auto* limit = numbers(scalar, scaleExponentLimit);
            auto* negativeLimit = ir_.CreateFNeg(limit);
            safe = ir_.CreateSelect(ir_.CreateFCmpOGT(safe, limit), limit, safe);
            safe = ir_.CreateSelect(ir_.CreateFCmpOLT(safe, negativeLimit), negativeLimit, safe);
            auto* exponent = ir_.CreateFPToSI(safe, lanes(ir_.getInt32Ty()));
            auto* scaled = ir_.CreateIntrinsic(llvm::Intrinsic::ldexp, {a->getType(), exponent->getType()},
                                               {a, exponent});
            auto* special = ir_.CreateFMul(a, ir_.CreateSelect(ir_.CreateFCmpOEQ(b, negativeInfinity), zero, b));
            return ir_.CreateSelect(finite, scaled, special);
        }
        default: std::abort();
        }
    }

    llvm::Value* shifted(DspOp op, DspType type, llvm::Value* bits, Register right)
    {
        auto* integer = type == DspType::Float || type == DspType::Int ? ir_.getInt32Ty() : ir_.getInt64Ty();
        auto* value = ir_.CreateTruncOrBitCast(bits, lanes(integer));
        auto* count = asDouble(right);
        auto* zero = numbers(ir_.getDoubleTy(), 0);
        auto* valid = ir_.CreateAnd(ir_.CreateFCmpOGE(count, zero),
            ir_.CreateFCmpOLT(count, numbers(ir_.getDoubleTy(), integer->getBitWidth())));
        auto* safe = ir_.CreateSelect(valid, count, zero);
        auto* amount = ir_.CreateFPToUI(safe, lanes(integer));
        llvm::Value* result;
        if (op == DspOp::ShiftArithmetic) {
            amount = ir_.CreateSelect(valid, amount, integers(integer, integer->getBitWidth() - 1));
            result = ir_.CreateAShr(value, amount);
        } else {
            auto* shifted = op == DspOp::ShiftLeft ? ir_.CreateShl(value, amount) : ir_.CreateLShr(value, amount);
            result = ir_.CreateSelect(valid, shifted, integers(integer, 0));
        }
        return ir_.CreateZExtOrBitCast(result, bitsType());
    }

    llvm::Value* isSseNaN(llvm::Value* bits, DspType type)
    {
        const auto format = sseFloatFormat(type);
        const auto magnitudeMask = type == DspType::Double ? ~format.sign : 0x7fffffffULL;
        auto* magnitude = ir_.CreateAnd(bits, integers(ir_.getInt64Ty(), magnitudeMask));
        return ir_.CreateICmpUGT(magnitude, integers(ir_.getInt64Ty(), format.infinity));
    }

    llvm::Value* sseArithmetic(DspOp op, Register left, Register right)
    {
        const auto format = sseFloatFormat(left.type);
        auto* quietBit = integers(ir_.getInt64Ty(), format.quiet);
        auto* zero = integers(ir_.getInt64Ty(), 0);
        auto* leftNaN = isSseNaN(left.bits, left.type);
        auto* rightNaN = isSseNaN(right.bits, left.type);
        auto* leftQuiet = ir_.CreateICmpNE(ir_.CreateAnd(left.bits, quietBit), zero);
        auto* rightSignaling = ir_.CreateICmpEQ(ir_.CreateAnd(right.bits, quietBit), zero);
        auto* useRight = ir_.CreateAnd(rightNaN, ir_.CreateOr(ir_.CreateNot(leftNaN), ir_.CreateAnd(leftQuiet, rightSignaling)));
        auto* sourceNaN = ir_.CreateOr(ir_.CreateSelect(useRight, right.bits, left.bits), quietBit);
        auto* a = floatValue(left);
        auto* b = floatValue(right);
        llvm::Value* value;
        switch (op) {
        case DspOp::SseAdd: value = ir_.CreateFAdd(a, b); break;
        case DspOp::SseSubtract: value = ir_.CreateFSub(a, b); break;
        case DspOp::SseMultiply: value = ir_.CreateFMul(a, b); break;
        case DspOp::SseDivide: value = ir_.CreateFDiv(a, b); break;
        default: std::abort();
        }
        auto* result = floatingBits(value);
        auto* numeric = ir_.CreateSelect(isSseNaN(result, left.type), integers(ir_.getInt64Ty(), format.infinity | format.quiet), result);
        return ir_.CreateSelect(ir_.CreateOr(leftNaN, rightNaN), sourceNaN, numeric);
    }

    Register sseBinary(const DspInstruction& op, Register left, Register right)
    {
        if (op.op == DspOp::MultiplyUnsignedDoublewords) {
            auto* a = wide(raw32(left.bits));
            auto* b = wide(raw32(right.bits));
            auto* products = ir_.CreateMul(a, b);
            auto* high = ir_.CreateLShr(products, integers(ir_.getInt64Ty(), 32));
            return {op.type, ir_.CreateShuffleVector(wide(raw32(products)), high, {0, 4, 2, 6})};
        }
        if (op.op == DspOp::ShufflePs) {
            std::array<int, laneCount> shuffle;
            for (unsigned lane = 0; lane < laneCount; ++lane) {
                shuffle[lane] = static_cast<int>((op.bits >> (2 * lane)) & 3U) + (lane < 2 ? 0 : 4);
            }
            return {op.type, ir_.CreateShuffleVector(left.bits, right.bits, shuffle)};
        }
        if (op.op == DspOp::ShufflePd) {
            llvm::Value* result = llvm::Constant::getNullValue(bitsType());
            result = insert(result, extract(left.bits, op.bits & 1U), 0);
            result = insert(result, extract(right.bits, (op.bits >> 1) & 1U), 1);
            return {op.type, result};
        }
        if (isSseArithmetic(op.op)) {
            return {op.type, sseArithmetic(op.op, left, right)};
        }
        auto* a = floatValue(left);
        auto* b = floatValue(right);
        if (op.op == DspOp::SseCompare) {
            constexpr llvm::CmpInst::Predicate predicates[]{
                llvm::CmpInst::FCMP_OEQ, llvm::CmpInst::FCMP_OLT, llvm::CmpInst::FCMP_OLE, llvm::CmpInst::FCMP_UNO,
                llvm::CmpInst::FCMP_UNE, llvm::CmpInst::FCMP_UGE, llvm::CmpInst::FCMP_UGT, llvm::CmpInst::FCMP_ORD,
            };
            auto* bits = masks(ir_.CreateFCmp(predicates[op.bits], a, b));
            return {op.type, op.type == DspType::Double ? bits : wide(raw32(bits))};
        }
        auto* chooseLeft = op.op == DspOp::SseMin ? ir_.CreateFCmpOLT(a, b) : ir_.CreateFCmpOGT(a, b);
        return {op.type, ir_.CreateSelect(chooseLeft, left.bits, right.bits)};
    }

    Register binary(const DspInstruction& op, Register left, Register right)
    {
        const auto type = binaryType(op.op, left.type, right.type, op.type);
        if (isShift(op.op)) {
            return {type, shifted(op.op, type, left.type == DspType::Bool ? left.bits : convert(left, type), right)};
        }
        if (isBitwise(op.op)) {
            auto* a = left.type == DspType::Bool ? left.bits : convert(left, type);
            auto* b = right.type == DspType::Bool ? right.bits : convert(right, type);
            auto* bits = op.op == DspOp::And ? ir_.CreateAnd(a, b)
                : op.op == DspOp::Or ? ir_.CreateOr(a, b) : ir_.CreateXor(a, b);
            return {type, type == DspType::Float || type == DspType::Int ? wide(raw32(bits)) : bits};
        }
        const Register a{type, convert(left, type)};
        const Register b{type, convert(right, type)};
        if (isComparison(op.op)) {
            return {DspType::Bool, masks(comparison(op.op, asDouble(a), asDouble(b)))};
        }
        if (type == DspType::Int) {
            if (op.op == DspOp::Power) {
                return {type, asInt(libm("pow", asDouble(a), asDouble(b)))};
            }
            return {type, wide(integerBinary(op.op, raw32(a.bits), raw32(b.bits)))};
        }
        return {type, floatingBits(floatingBinary(op.op, floatValue(a), floatValue(b)))};
    }

    llvm::Value* floatingUnary(DspOp op, llvm::Value* value)
    {
        auto* scalar = value->getType()->getScalarType();
        switch (op) {
        case DspOp::Negate: return ir_.CreateFNeg(value);
        case DspOp::SinRadians: return libm("sin", value);
        case DspOp::CosRadians: return libm("cos", value);
        case DspOp::TanRadians: return libm("tan", value);
        case DspOp::Sin1: case DspOp::Cos1: case DspOp::Tan1: {
            const double turn = scalar->isFloatTy() ? 2.0F * std::numbers::pi_v<float> : 2.0 * std::numbers::pi;
            const auto name = op == DspOp::Sin1 ? "sin" : op == DspOp::Cos1 ? "cos" : "tan";
            return libm(name, ir_.CreateFMul(value, numbers(scalar, turn)));
        }
        case DspOp::Sign: {
            auto* zero = numbers(scalar, 0);
            return ir_.CreateSelect(ir_.CreateFCmpOGT(value, zero), numbers(scalar, 1),
                ir_.CreateSelect(ir_.CreateFCmpOLT(value, zero), numbers(scalar, -1), zero));
        }
        case DspOp::Abs: return libm("fabs", value);
        case DspOp::Exp: return libm("exp", value);
        case DspOp::Exp2MinusOne: {
            const double ln2 = scalar->isFloatTy() ? std::numbers::ln2_v<float> : std::numbers::ln2;
            return libm("expm1", ir_.CreateFMul(value, numbers(scalar, ln2)));
        }
        case DspOp::Sqrt: return libm("sqrt", value);
        case DspOp::Log10: return libm("log10", value);
        case DspOp::Log2: return libm("log2", value);
        case DspOp::Ln: return libm("log", value);
        case DspOp::RoundNearest: return libm("nearbyint", value);
        case DspOp::Round: return libm("round", value);
        case DspOp::Trunc: return libm("trunc", value);
        case DspOp::Floor: return libm("floor", value);
        case DspOp::Ceil: return libm("ceil", value);
        default: std::abort();
        }
    }

    llvm::Value* flushSubnormal(DspType type, llvm::Value* bits)
    {
        const std::uint64_t exponent = type == DspType::Float ? 0x7f800000U : 0x7ff0000000000000ULL;
        const std::uint64_t sign = type == DspType::Float ? 0x80000000U : 0x8000000000000000ULL;
        auto* magnitude = ir_.CreateAnd(bits, integers(ir_.getInt64Ty(), exponent));
        auto* tiny = ir_.CreateICmpEQ(magnitude, integers(ir_.getInt64Ty(), 0));
        return ir_.CreateSelect(tiny, ir_.CreateAnd(bits, integers(ir_.getInt64Ty(), sign)), bits);
    }

    Register unary(const DspInstruction& op, Register source)
    {
        if (op.op == DspOp::Negate && (source.type == DspType::Decimal || source.type == DspType::Fraction)) {
            return {source.type, ir_.CreateXor(source.bits, integers(ir_.getInt64Ty(), std::uint64_t{1} << 63))};
        }
        const auto type = unaryType(op.op, source.type, op.type);
        auto* bits = convert(source, type);
        if (op.op == DspOp::Convert) {
            return {type, bits};
        }
        if (op.op == DspOp::Complement) {
            auto* inverted = ir_.CreateNot(bits);
            return {type, type == DspType::Float || type == DspType::Int ? wide(raw32(inverted)) : inverted};
        }
        if (op.op == DspOp::Denorm) {
            return {type, flushSubnormal(type, bits)};
        }
        if (op.op == DspOp::SseSqrt) {
            auto* input = floatValue({type, bits});
            auto* root = floatingBits(ir_.CreateIntrinsic(llvm::Intrinsic::sqrt, {input->getType()}, {input}));
            const auto format = sseFloatFormat(type);
            auto* invalid = integers(ir_.getInt64Ty(), format.sign | format.infinity | format.quiet);
            auto* result = ir_.CreateSelect(isSseNaN(root, type), invalid, root);
            auto* quieted = ir_.CreateOr(bits, integers(ir_.getInt64Ty(), format.quiet));
            return {type, ir_.CreateSelect(isSseNaN(bits, type), quieted, result)};
        }
        if (type == DspType::Int) {
            auto* number = raw32(bits);
            auto* zero = integers(ir_.getInt32Ty(), 0);
            auto* negative = ir_.CreateICmpSLT(number, zero);
            switch (op.op) {
            case DspOp::Negate: return {type, wide(ir_.CreateNeg(number))};
            case DspOp::Abs: return {type, wide(ir_.CreateSelect(negative, ir_.CreateNeg(number), number))};
            case DspOp::Sign:
                return {type, wide(ir_.CreateSelect(negative, integers(ir_.getInt32Ty(), ~std::uint32_t{}),
                    ir_.CreateZExt(ir_.CreateICmpSGT(number, zero), lanes(ir_.getInt32Ty()))))};
            default: std::abort();
            }
        }
        return {type, floatingBits(floatingUnary(op.op, floatValue({type, bits})))};
    }

    Register pop()
    {
        const auto value = stack_.back();
        stack_.pop_back();
        return value;
    }

    void lower(const DspInstruction& op, std::size_t pc)
    {
        switch (op.op) {
        case DspOp::Constant:
            stack_.push_back({op.type, integers(ir_.getInt64Ty(), op.bits)});
            break;
        case DspOp::X87Load: {
            const auto value = pop();
            auto* signature = llvm::FunctionType::get(ir_.getVoidTy(), {ir_.getPtrTy(), ir_.getInt64Ty(), ir_.getInt64Ty()}, false);
            ir_.CreateCall(module_.getOrInsertFunction("openstone_dsp_x87_load", signature),
                {slot(op.slot), extract(value.bits, 0), ir_.getInt64(static_cast<std::uint64_t>(op.type))});
            break;
        }
        case DspOp::X87Store: {
            auto* signature = llvm::FunctionType::get(ir_.getInt64Ty(), {ir_.getPtrTy(), ir_.getInt64Ty()}, false);
            auto* bits = ir_.CreateCall(module_.getOrInsertFunction("openstone_dsp_x87_store", signature),
                {slot(op.slot), ir_.getInt64(static_cast<std::uint64_t>(op.type))});
            stack_.push_back({op.type, ir_.CreateVectorSplat(laneCount, bits)});
            break;
        }
        case DspOp::X87Copy:
            ir_.CreateAlignedStore(load(slot(op.count)), slot(op.slot), llvm::Align(alignof(DspLanes)));
            break;
        case DspOp::X87Exchange: {
            auto* a = load(slot(op.slot));
            auto* b = load(slot(op.count));
            ir_.CreateAlignedStore(b, slot(op.slot), llvm::Align(alignof(DspLanes)));
            ir_.CreateAlignedStore(a, slot(op.count), llvm::Align(alignof(DspLanes)));
            break;
        }
        case DspOp::X87Unary: {
            auto* signature = llvm::FunctionType::get(ir_.getVoidTy(), {ir_.getPtrTy(), ir_.getInt64Ty()}, false);
            ir_.CreateCall(module_.getOrInsertFunction("openstone_dsp_x87_unary", signature), {slot(op.slot), ir_.getInt64(op.bits)});
            break;
        }
        case DspOp::X87Binary: {
            auto* signature = llvm::FunctionType::get(ir_.getVoidTy(), {ir_.getPtrTy(), ir_.getPtrTy(), ir_.getInt64Ty()}, false);
            ir_.CreateCall(module_.getOrInsertFunction("openstone_dsp_x87_binary", signature), {slot(op.slot), slot(op.count), ir_.getInt64(op.bits)});
            break;
        }
        case DspOp::Load:
            stack_.push_back({op.type, selectLanes(load(slot(op.slot)), op.lanes)});
            break;
        case DspOp::LoadArrayPacket:
            stack_.push_back({op.type, selectLanes(load(ir_.CreateGEP(bitsType(), arrays_, ir_.getInt64(op.slot))), op.lanes)});
            break;
        case DspOp::Store: {
            auto* value = broadcast(convert(pop(), op.type), op.lanes.broadcast);
            auto* address = slot(op.slot);
            if (op.lanes.mask != 0xf) {
                value = ir_.CreateSelect(laneMask(op.lanes.mask), value, load(address));
            }
            ir_.CreateAlignedStore(value, address, llvm::Align(alignof(DspLanes)));
            break;
        }
        case DspOp::LoadIndexed: case DspOp::LoadArrayInput: {
            const auto index = pop();
            auto* base = op.op == DspOp::LoadIndexed ? slots_ : arrays_;
            stack_.push_back({op.type, loadIndexed(base, index, op)});
            break;
        }
        case DspOp::LoadAssemblyMemory: {
            const auto index = pop();
            stack_.push_back({op.type, loadAssemblyMemory(index, op)});
            break;
        }
        case DspOp::StoreAssemblyMemory: {
            const auto index = pop();
            storeAssemblyMemory(index, pop(), op);
            break;
        }
        case DspOp::LoadAssemblyByte: {
            const auto index = pop();
            stack_.push_back({DspType::Int, loadAssemblyByte(index, op)});
            break;
        }
        case DspOp::StoreAssemblyByte: {
            const auto index = pop();
            storeAssemblyByte(index, pop(), op);
            break;
        }
        case DspOp::LoadAssemblyPacket: {
            const auto index = pop();
            stack_.push_back({op.type, loadAssemblyPacket(index, op)});
            break;
        }
        case DspOp::StoreAssemblyPacket: {
            const auto index = pop();
            storeAssemblyPacket(index, pop(), op);
            break;
        }
        case DspOp::Reinterpret128:
            stack_.back() = reinterpret128(stack_.back(), op.type);
            stack_.back().bits = selectLanes(stack_.back().bits, op.lanes);
            break;
        case DspOp::LoadMemRef: case DspOp::LoadMemArrayField: case DspOp::LoadMemByte: {
            const auto index = pop();
            stack_.push_back({op.type, loadMemRef(index, op)});
            break;
        }
        case DspOp::LoadEnginePointer: {
            const auto index = pop();
            auto* signature = llvm::FunctionType::get(ir_.getInt64Ty(), {ir_.getPtrTy(), ir_.getInt64Ty(), ir_.getInt64Ty(), ir_.getInt64Ty()}, false);
            auto* offset = ir_.CreateSExt(extract(raw32(index.bits), 0), ir_.getInt64Ty());
            auto* pointer = ir_.CreateCall(module_.getOrInsertFunction("openstone_dsp_load_engine_pointer", signature), {references_, ir_.getInt64(op.slot), engineBase(op), offset});
            stack_.push_back({DspType::Int, ir_.CreateVectorSplat(laneCount, pointer)});
            break;
        }
        case DspOp::MemRefPresent: {
            auto* signature = llvm::FunctionType::get(ir_.getInt64Ty(), {ir_.getPtrTy(), ir_.getInt64Ty()}, false);
            auto* present = ir_.CreateCall(module_.getOrInsertFunction("openstone_dsp_memref_present", signature), {references_, ir_.getInt64(op.slot)});
            stack_.push_back({DspType::Int, ir_.CreateVectorSplat(laneCount, present)});
            break;
        }
        case DspOp::IsMemRefTarget: {
            const auto index = pop();
            auto* signature = llvm::FunctionType::get(ir_.getInt64Ty(),
                {ir_.getPtrTy(), ir_.getInt64Ty(), ir_.getInt64Ty()}, false);
            const auto function = module_.getOrInsertFunction("openstone_dsp_is_memref_target", signature);
            auto* offset = ir_.CreateSExt(extract(raw32(index.bits), 0), ir_.getInt64Ty());
            auto* valid = ir_.CreateCall(function, {references_, ir_.getInt64(op.slot), offset});
            stack_.push_back({DspType::Bool, masks(ir_.CreateICmpNE(ir_.CreateVectorSplat(4, valid), integers(ir_.getInt64Ty(), 0)))});
            break;
        }
        case DspOp::StoreMemRef: case DspOp::StoreMemByte: {
            const auto index = pop();
            storeMemRef(index, pop(), op);
            break;
        }
        case DspOp::StoreIndexed: {
            const auto value = pop();
            storeIndexed(pop(), value, op);
            break;
        }
        case DspOp::Raw32:
            stack_.back() = {op.type, wide(raw32(stack_.back().bits))};
            break;
        case DspOp::SseMin: case DspOp::SseMax: case DspOp::SseCompare: case DspOp::ShufflePs: case DspOp::ShufflePd: case DspOp::SseAdd: case DspOp::SseSubtract: case DspOp::SseMultiply: case DspOp::SseDivide:
        case DspOp::MultiplyUnsignedDoublewords: {
            const auto right = pop();
            stack_.back() = sseBinary(op, stack_.back(), right);
            break;
        }
        case DspOp::IntegerDivide: {
            const bool byte = op.bits == 8;
            auto* rawDivisor = extract(raw32(pop().bits), 0);
            auto* divisor = ir_.CreateSExt(byte ? ir_.CreateTrunc(rawDivisor, ir_.getInt8Ty()) : rawDivisor, ir_.getInt64Ty());
            auto* high = ir_.CreateZExt(extract(raw32(pop().bits), 0), ir_.getInt64Ty());
            auto* low = ir_.CreateZExt(extract(raw32(pop().bits), 0), ir_.getInt64Ty());
            auto* dividend = byte ? ir_.CreateSExt(ir_.CreateTrunc(low, ir_.getInt16Ty()), ir_.getInt64Ty())
                : ir_.CreateOr(ir_.CreateShl(high, 32), low);
            auto* zero = ir_.CreateICmpEQ(divisor, ir_.getInt64(0));
            auto* wideOverflow = ir_.CreateAnd(ir_.CreateICmpEQ(dividend, ir_.getInt64(0x8000000000000000ULL)),
                                              ir_.CreateICmpEQ(divisor, ir_.getInt64(UINT64_MAX)));
            // Sanitize before SDIV/SREM, so even speculative/optimized evaluation cannot create LLVM poison.
            auto* safeDivisor = ir_.CreateSelect(ir_.CreateOr(zero, wideOverflow), ir_.getInt64(1), divisor);
            auto* quotient = ir_.CreateSDiv(dividend, safeDivisor);
            auto* remainder = ir_.CreateSRem(dividend, safeDivisor);
            auto* fits = ir_.CreateAnd(ir_.CreateICmpSGE(quotient, ir_.getInt64(byte ? std::uint64_t{0xffffffffffffff80ULL} : std::uint64_t{0xffffffff80000000ULL})),
                                      ir_.CreateICmpSLE(quotient, ir_.getInt64(byte ? 127 : 0x7fffffff)));
            auto* valid = ir_.CreateAnd(ir_.CreateNot(ir_.CreateOr(zero, wideOverflow)), fits);
            auto* success = block("idiv.success");
            auto* fault = block("idiv.fault");
            ir_.CreateCondBr(valid, success, fault);
            ir_.SetInsertPoint(fault);
            auto* signature = llvm::FunctionType::get(ir_.getVoidTy(), {ir_.getPtrTy(),ir_.getInt64Ty(),ir_.getInt64Ty()}, false);
            auto* reason = ir_.CreateSelect(zero, ir_.getInt64(static_cast<std::uint64_t>(DspFaultKind::IntegerDivideByZero)),
                                           ir_.getInt64(static_cast<std::uint64_t>(DspFaultKind::IntegerDivideOverflow)));
            ir_.CreateCall(module_.getOrInsertFunction("openstone_dsp_divide_fault", signature), {references_,reason,frame_});
            ir_.CreateRetVoid();
            ir_.SetInsertPoint(success);
            stack_.push_back({DspType::Int, ir_.CreateVectorSplat(laneCount, ir_.CreateZExt(ir_.CreateTrunc(quotient,ir_.getInt32Ty()),ir_.getInt64Ty()))});
            stack_.push_back({DspType::Int, ir_.CreateVectorSplat(laneCount, ir_.CreateZExt(ir_.CreateTrunc(remainder,ir_.getInt32Ty()),ir_.getInt64Ty()))});
            break;
        }
        case DspOp::IntegerCarry: {
            const auto right = pop(), left = pop();
            auto* a = raw32(left.bits);
            auto* b = raw32(right.bits);
            auto* carry = op.bits ? ir_.CreateICmpULT(ir_.CreateAdd(a, b), a) : ir_.CreateICmpULT(a, b);
            stack_.push_back({DspType::Bool, masks(carry)});
            break;
        }
        case DspOp::IntegerParity: {
            auto& value = stack_.back();
            auto* byte = ir_.CreateAnd(raw32(value.bits), integers(ir_.getInt32Ty(), 0xff));
            auto* count = ir_.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, byte);
            auto* odd = ir_.CreateAnd(count, integers(ir_.getInt32Ty(), 1));
            value = {DspType::Bool, masks(ir_.CreateICmpEQ(odd, integers(ir_.getInt32Ty(), 0)))};
            break;
        }
        case DspOp::Duplicate:
            stack_.push_back(stack_.back());
            break;
        case DspOp::PackedShiftCount: {
            auto& value = stack_.back();
            auto* words = raw32(value.bits);
            auto* low = extract(words, 0);
            auto* high = extract(words, 1);
            auto* valid = ir_.CreateAnd(ir_.CreateICmpEQ(high, ir_.getInt32(0)), ir_.CreateICmpULT(low, ir_.getInt32(32)));
            auto* count = ir_.CreateSelect(valid, low, ir_.getInt32(32));
            value.bits = ir_.CreateVectorSplat(laneCount, ir_.CreateZExt(count, ir_.getInt64Ty()));
            break;
        }
        case DspOp::Loop:
            ir_.CreateStore(ir_.getInt64(op.count), loops_[pc]);
            ir_.CreateBr(leaders_[op.count == 0 ? op.slot : pc + 1]);
            break;
        case DspOp::Repeat: {
            auto* counter = loops_[op.slot - 1];
            auto* remaining = ir_.CreateSub(ir_.CreateLoad(ir_.getInt64Ty(), counter), ir_.getInt64(1));
            ir_.CreateStore(remaining, counter);
            ir_.CreateCondBr(ir_.CreateICmpNE(remaining, ir_.getInt64(0)), leaders_[op.slot], leaders_[pc + 1]);
            break;
        }
        case DspOp::Hop: {
            auto* phase = ir_.CreateAnd(frame_, ir_.getInt64(op.count - 1));
            ir_.CreateCondBr(ir_.CreateICmpEQ(phase, ir_.getInt64(0)), leaders_[pc + 1], leaders_[op.slot]);
            break;
        }
        case DspOp::JumpIfTrue:
        case DspOp::Jump: {
            auto* target = leaders_[op.slot];
            auto* condition = op.op == DspOp::JumpIfTrue ? extract(truth(pop()), 0) : nullptr;
            if (op.slot > pc) {
                if (condition) ir_.CreateCondBr(condition, target, leaders_[pc + 1]);
                else ir_.CreateBr(target);
                break;
            }
            if (condition) {
                auto* taken = block("backward.branch");
                ir_.CreateCondBr(condition, taken, leaders_[pc + 1]);
                ir_.SetInsertPoint(taken);
            }
            auto* count = ir_.CreateLoad(ir_.getInt32Ty(), backwardBranches_);
            ir_.CreateStore(ir_.CreateAdd(count, ir_.getInt32(1)), backwardBranches_);
            ir_.CreateCondBr(ir_.CreateICmpULT(count, ir_.getInt32(dspBackwardBranchLimit)), target, leaders_.back());
            break;
        }
        case DspOp::LocalCall: {
            auto* count = ir_.CreateLoad(ir_.getInt32Ty(), recursiveCalls_);
            auto* call = block("local.call");
            ir_.CreateCondBr(ir_.CreateICmpULT(count, ir_.getInt32(dspRecursiveCallLimit)), call, leaders_.back());
            ir_.SetInsertPoint(call);
            ir_.CreateStore(ir_.CreateAdd(count, ir_.getInt32(1)), recursiveCalls_);
            if (auto* continuation = subroutines_.at(op.slot).savedContinuation) {
                ir_.CreateStore(ir_.getInt64(pc + 1), continuation);
            }
            ir_.CreateBr(leaders_[op.slot]);
            break;
        }
        case DspOp::LocalReturn: {
            const auto& subroutine = subroutines_.at(op.slot);
            if (!subroutine.savedContinuation) {
                ir_.CreateBr(leaders_[subroutine.continuations.front()]);
                break;
            }
            auto* continuation = ir_.CreateLoad(ir_.getInt64Ty(), subroutine.savedContinuation);
            auto* branch = ir_.CreateSwitch(continuation, leaders_.back(),
                                            static_cast<unsigned>(subroutine.continuations.size()));
            for (const auto returnAddress : subroutine.continuations) {
                branch->addCase(ir_.getInt64(returnAddress), leaders_[returnAddress]);
            }
            break;
        }
        case DspOp::BindNativeFrame: {
            auto* zero = llvm::Constant::getNullValue(bitsType());
            ir_.CreateAlignedStore(zero, slot(op.slot), llvm::Align(alignof(DspLanes)));
            ir_.CreateAlignedStore(zero, slot(op.count), llvm::Align(alignof(DspLanes)));
            break;
        }
        case DspOp::ReadSamplePosition: {
            auto* signature = llvm::FunctionType::get(ir_.getInt64Ty(), {references_->getType()}, false);
            auto* value = ir_.CreateCall(module_.getOrInsertFunction("openstone_dsp_sample_position", signature), {references_});
            ir_.CreateAlignedStore(ir_.CreateVectorSplat(laneCount, value), slot(op.slot), llvm::Align(alignof(DspLanes)));
            break;
        }
        case DspOp::ReadTimestamp: {
            auto* signature = llvm::FunctionType::get(ir_.getInt64Ty(), false);
            auto* ticks = ir_.CreateCall(module_.getOrInsertFunction("openstone_dsp_timestamp", signature));
            auto* low = ir_.CreateAnd(ticks, ir_.getInt64(0xffffffffU));
            auto* high = ir_.CreateLShr(ticks, ir_.getInt64(32));
            ir_.CreateAlignedStore(ir_.CreateVectorSplat(laneCount, low), slot(op.slot), llvm::Align(alignof(DspLanes)));
            ir_.CreateAlignedStore(ir_.CreateVectorSplat(laneCount, high), slot(op.count), llvm::Align(alignof(DspLanes)));
            break;
        }
        case DspOp::Select: {
            const auto no = pop();
            const auto yes = pop();
            const auto condition = pop();
            const auto type = commonType(yes.type, no.type, op.type);
            stack_.push_back({type, ir_.CreateSelect(truth(condition), convert(yes, type), convert(no, type))});
            break;
        }
        case DspOp::Negate: case DspOp::Complement: case DspOp::Sin1: case DspOp::Cos1: case DspOp::Tan1:
        case DspOp::Log10: case DspOp::Log2: case DspOp::Ln: case DspOp::Sign: case DspOp::Abs:
        case DspOp::Exp: case DspOp::Sqrt: case DspOp::RoundNearest: case DspOp::Round: case DspOp::Trunc:
        case DspOp::Floor: case DspOp::Ceil: case DspOp::Convert: case DspOp::Denorm: case DspOp::Exp2MinusOne:
        case DspOp::SseSqrt: case DspOp::SinRadians: case DspOp::CosRadians: case DspOp::TanRadians:
            stack_.back() = unary(op, stack_.back());
            break;
        default: {
            const auto right = pop();
            stack_.back() = binary(op, stack_.back(), right);
            break;
        }
        }
    }

    llvm::Module& module_;
    llvm::IRBuilder<> ir_;
    std::span<const DspInstruction> code_;
    std::vector<llvm::BasicBlock*> leaders_;
    std::vector<llvm::AllocaInst*> loops_;
    llvm::AllocaInst* backwardBranches_{};
    llvm::AllocaInst* recursiveCalls_{};
    std::unordered_map<std::size_t, LocalSubroutine> subroutines_;
    std::vector<Register> stack_;
    llvm::Function* function_{};
    llvm::Argument* slots_{};
    llvm::Argument* arrays_{};
    llvm::Argument* references_{};
    llvm::Argument* frame_{};
};

void verify(const llvm::Module& module)
{
    std::string message;
    llvm::raw_string_ostream output(message);
    if (llvm::verifyModule(module, &output)) {
        throw std::runtime_error("LLVM rejected the generated DSP IR: " + output.str());
    }
}

void optimize(llvm::Module& module)
{
    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager callGraph;
    llvm::ModuleAnalysisManager modules;
    llvm::PassBuilder passes;
    passes.registerModuleAnalyses(modules);
    passes.registerCGSCCAnalyses(callGraph);
    passes.registerFunctionAnalyses(functions);
    passes.registerLoopAnalyses(loops);
    passes.crossRegisterProxies(loops, functions, callGraph, modules);
    passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2).run(module, modules);
}

void requireSuccess(llvm::Error error)
{
    if (error) {
        throw std::runtime_error("LLVM ORC: " + llvm::toString(std::move(error)));
    }
}

template<typename T>
T requireValue(llvm::Expected<T> result)
{
    if (!result) {
        requireSuccess(result.takeError());
    }
    return std::move(*result);
}

}

std::shared_ptr<const DspNativeProgram> compileDspOrc(const DspProgramData& program)
{
    static const bool initialized = [] {
        return !llvm::InitializeNativeTarget() && !llvm::InitializeNativeTargetAsmPrinter();
    }();
    if (!initialized) {
        throw std::runtime_error("LLVM ORC could not initialize the native target");
    }

    auto engine = requireValue(llvm::orc::LLJITBuilder().create());
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("openstone_dsp", *context);
    module->setDataLayout(engine->getDataLayout());
    module->setTargetTriple(engine->getTargetTriple());

    const std::array names{"openstone_dsp_stage0", "openstone_dsp_stage1", "openstone_dsp_stage2", "openstone_dsp_stage3"};
    for (std::size_t stage = 0; stage < program.stages.size(); ++stage) {
        if (!program.stages[stage].empty()) {
            StageLowerer(*module, names[stage], program.stages[stage]).lower();
        }
    }
    verify(*module);
    optimize(*module);
    llvm::orc::SymbolMap symbols;
    const auto registerHelper = [&](const char* name, auto* function) {
        if (module->getFunction(name)) {
            symbols[engine->mangleAndIntern(name)] =
                llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(function), llvm::JITSymbolFlags::Exported);
        }
    };
    registerHelper("openstone_dsp_divide_fault", &dspReportDivideFault);
    registerHelper("openstone_dsp_timestamp", &dspTimestamp);
    registerHelper("openstone_dsp_sample_position", &dspSamplePosition);
    registerHelper("openstone_dsp_x87_load", &dspX87Load);
    registerHelper("openstone_dsp_x87_store", &dspX87Store);
    registerHelper("openstone_dsp_x87_unary", &dspX87Unary);
    registerHelper("openstone_dsp_x87_binary", &dspX87Binary);
    registerHelper("openstone_dsp_read_memref", &dspReadMemRef);
    registerHelper("openstone_dsp_read_memarray_field", &dspReadMemArrayField);
    registerHelper("openstone_dsp_write_memref", &dspWriteMemRef);
    registerHelper("openstone_dsp_is_memref_target", &dspIsMemRefTarget);
    registerHelper("openstone_dsp_memref_present", &dspMemRefPresent);
    registerHelper("openstone_dsp_load_engine_pointer", &dspLoadEnginePointer);
    registerHelper("openstone_dsp_engine_offset", &dspEngineOffset);
    if (!symbols.empty()) {
        requireSuccess(engine->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols))));
    }
    auto compiled = std::make_shared<DspNativeProgram>();
    {
        llvm::raw_string_ostream output(compiled->optimizedIr);
        module->print(output, nullptr);
    }
    requireSuccess(engine->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));

    for (std::size_t stage = 0; stage < program.stages.size(); ++stage) {
        if (!program.stages[stage].empty()) {
            compiled->stages[stage] = requireValue(engine->lookup(names[stage])).toPtr<DspStageEntry>();
        }
    }
    compiled->lifetime = std::shared_ptr<llvm::orc::LLJIT>(std::move(engine));
    return compiled;
}

}
