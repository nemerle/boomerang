/*
 * Copyright (C) 2004-2006, Mike Van Emmerik
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 */

/***************************************************************************/ /**
  * \file       dfa.cpp
  * \brief   Implementation of class Type functions related to solving type analysis in an iterative, data-flow-based
  *                manner
  ******************************************************************************/

#include "config.h"

#define NO_GARBAGE_COLLECTOR
#include "type.h"
#include "boomerang.h"
#include "signature.h"
#include "exp.h"
#include "prog.h"
#include "visitor.h"
#include "log.h"
#include "proc.h"
#include "util.h"

#include <sstream>
#include <cstring>
#include <utility>
#include <QDebug>

static int nextUnionNumber = 0;

#define DFA_ITER_LIMIT 20

// idx + K; leave idx wild
static const Binary unscaledArrayPat(opPlus, Terminal::get(opWild), Terminal::get(opWildIntConst));


static int dfa_progress = 0;
void UserProc::dfaTypeAnalysis() {
    Boomerang::get()->alertDecompileDebugPoint(this, "before dfa type analysis");

    // First use the type information from the signature. Sometimes needed to split variables (e.g. argc as a
    // int and char* in sparc/switch_gcc)
    bool ch = signature->dfaTypeAnalysis(cfg);
    StatementList stmts;
    getStatements(stmts);

    int iter;
    for (iter = 1; iter <= DFA_ITER_LIMIT; ++iter) {
        ch = false;
        for (Instruction *it : stmts) {
            if (++dfa_progress >= 2000) {
                dfa_progress = 0;
                LOG_STREAM() << "t";
                LOG_STREAM().flush();
            }
            bool thisCh = false;
            Instruction *before = nullptr;
            if (DEBUG_TA)
                before = it->clone();
            it->dfaTypeAnalysis(thisCh);
            if (thisCh) {
                ch = true;
                if (DEBUG_TA)
                    LOG << " caused change: FROM: " << before << "TO: \n" << it << "\n";
            }
            if (DEBUG_TA)
                delete before;
        }
        if (!ch)
            // No more changes: round robin algorithm terminates
            break;
    }
    if (ch)
        LOG << "### WARNING: iteration limit exceeded for dfaTypeAnalysis of procedure " << getName() << " ###\n";

    if (DEBUG_TA) {
        LOG << "\n ### results for data flow based type analysis for " << getName() << " ###\n";
        LOG << iter << " iterations\n";
        for (StatementList::iterator it = stmts.begin(); it != stmts.end(); ++it) {
            Instruction *s = *it;
            LOG << s << "\n"; // Print the statement; has dest type
            // Now print type for each constant in this Statement
            std::list<Const *> lc;
            std::list<Const *>::iterator cc;
            s->findConstants(lc);
            if (lc.size()) {
                LOG << "       ";
                for (cc = lc.begin(); cc != lc.end(); ++cc)
                    LOG << (*cc)->getType()->getCtype() << " " << *cc << "  ";
                LOG << "\n";
            }
            // If s is a call, also display its return types
            CallStatement *call = dynamic_cast<CallStatement *>(s);
            if (s->isCall() && call) {
                ReturnStatement *rs = call->getCalleeReturn();
                if (rs == nullptr)
                    continue;
                UseCollector *uc = call->getUseCollector();
                ReturnStatement::iterator rr;
                bool first = true;
                for (rr = rs->begin(); rr != rs->end(); ++rr) {
                    // Intersect the callee's returns with the live locations at the call, i.e. make sure that they
                    // exist in *uc
                    Assignment *assgn = dynamic_cast<Assignment *>(*rr);
                    Exp *lhs = assgn->getLeft();
                    if (!uc->exists(lhs))
                        continue; // Intersection fails
                    if (first)
                        LOG << "       returns: ";
                    else
                        LOG << ", ";
                    LOG << assgn->getType()->getCtype() << " " << assgn->getLeft();
                }
                LOG << "\n";
            }
        }
        LOG << "\n ### end results for Data flow based Type Analysis for " << getName() << " ###\n\n";
    }

// Now use the type information gathered
#if 0
    Boomerang::get()->alert_decompile_debug_point(this, "before mapping locals from dfa type analysis");
    if (DEBUG_TA)
        LOG << " ### mapping expressions to local variables for " << getName() << " ###\n";
    for (Statement *it : stmts) {
        s->dfaMapLocals();
    }
    if (DEBUG_TA)
        LOG << " ### end mapping expressions to local variables for " << getName() << " ###\n";
#endif

    Boomerang::get()->alertDecompileDebugPoint(this, "before other uses of dfa type analysis");
    debugPrintAll("before other uses of dfa type analysis");

    Prog *_prog = getProg();
    for (Instruction *s : stmts) {

        // 1) constants
        std::list<Const *> lc;
        s->findConstants(lc);
        std::list<Const *>::iterator cc;
        for (cc = lc.begin(); cc != lc.end(); ++cc) {
            Const *con = (Const *)*cc;
            if(con->getOper()==opStrConst)
                continue;
            SharedType t = con->getType();
            int val = con->getInt();
            if (t && t->resolvesToPointer()) {
                auto pt = t->as<PointerType>();
                SharedType baseType = pt->getPointsTo();
                if (baseType->resolvesToChar()) {
                    // Convert to a string    MVE: check for read-only?
                    // Also, distinguish between pointer to one char, and ptr to many?
                    const char *str = _prog->getStringConstant(ADDRESS::n(val), true);
                    if (str) {
                        // Make a string
                        con->setStr(str);
                        con->setOper(opStrConst);
                    }
                } else if (baseType->resolvesToInteger() || baseType->resolvesToFloat() || baseType->resolvesToSize()) {
                    ADDRESS addr = ADDRESS::g(con->getInt()); // TODO: use getAddr
                    _prog->globalUsed(addr, baseType);
                    QString gloName = _prog->getGlobalName(addr);
                    if (!gloName.isEmpty()) {
                        ADDRESS r = addr - _prog->getGlobalAddr(gloName);
                        Exp *ne;
                        if (!r.isZero()) { // TODO: what if r is NO_ADDR ?
                            Exp *g = Location::global(gloName, this);
                            ne = Location::memOf(Binary::get(opPlus, new Unary(opAddrOf, g), new Const(r)), this);
                        } else {
                            SharedType ty = _prog->getGlobalType(gloName);
                            Assign *assgn = dynamic_cast<Assign *>(s);
                            if (s->isAssign() && assgn && assgn->getType()) {
                                int bits = assgn->getType()->getSize();
                                if (ty == nullptr || ty->getSize() == 0)
                                    _prog->setGlobalType(gloName, IntegerType::get(bits));
                            }
                            Exp *g = Location::global(gloName, this);
                            if (ty && ty->resolvesToArray())
                                ne = Binary::get(opArrayIndex, g, new Const(0));
                            else
                                ne = g;
                        }
                        Exp *memof = Location::memOf(con);
                        if (!s->searchAndReplace(*memof, ne))
                            delete ne;
                    }
                } else if (baseType->resolvesToArray()) {
                    // We have found a constant in s which has type pointer to array of alpha. We can't get the parent
                    // of con, but we can find it with the pattern unscaledArrayPat.
                    std::list<Exp *> result;
                    s->searchAll(unscaledArrayPat, result);
                    for (auto &elem : result) {
                        // idx + K
                        Binary *bin_rr = dynamic_cast<Binary *>(elem);
                        assert(bin_rr);
                        Const *constK = (Const *)bin_rr->getSubExp2();
                        // Note: keep searching till we find the pattern with this constant, since other constants may
                        // not be used as pointer to array type.
                        if (constK != con)
                            continue;
                        ADDRESS K = ADDRESS::g(constK->getInt());
                        Exp *idx = bin_rr->getSubExp1();
                        Exp *arr = new Unary(
                            opAddrOf, Binary::get(opArrayIndex, Location::global(_prog->getGlobalName(K), this), idx));
                        // Beware of changing expressions in implicit assignments... map can become invalid
                        bool isImplicit = s->isImplicit();
                        if (isImplicit)
                            cfg->removeImplicitAssign(((ImplicitAssign *)s)->getLeft());
                        if (!s->searchAndReplace(unscaledArrayPat, arr))
                            delete arr; // remove if not emplaced in s
                        // s will likely have an m[a[array]], so simplify
                        s->simplifyAddr();
                        if (isImplicit)
                            // Replace the implicit assignment entry. Note that s' lhs has changed
                            cfg->findImplicitAssign(((ImplicitAssign *)s)->getLeft());
                        // Ensure that the global is declared
                        // Ugh... I think that arrays and pointers to arrays are muddled!
                        _prog->globalUsed(K, baseType);
                    }
                }
            } else if (t->resolvesToFloat()) {
                if (con->isIntConst()) {
                    // Reinterpret as a float (and convert to double)
                    // con->setFlt(reinterpret_cast<float>(con->getInt()));
                    int tmp = con->getInt();
                    con->setFlt(*(float *)&tmp); // Reinterpret to float, then cast to double
                    con->setOper(opFltConst);
                    con->setType(FloatType::get(64));
                }
                // MVE: more work if double?
            } else /* if (t->resolvesToArray()) */ {
                _prog->globalUsed(ADDRESS::n(val), t);
            }
        }

        // 2) Search for the scaled array pattern and replace it with an array use m[idx*K1 + K2]
        dfa_analyze_scaled_array_ref(s, _prog);

        // 3) Check implicit assigns for parameter and global types.
        dfa_analyze_implict_assigns(s, _prog);

        // 4) Add the locals (soon globals as well) to the localTable, to sort out the overlaps
        if (s->isTyping()) {
            Exp *addrExp = nullptr;
            SharedType typeExp = nullptr;
            if (s->isAssignment()) {
                Exp *lhs = ((Assignment *)s)->getLeft();
                if (lhs->isMemOf()) {
                    addrExp = ((Location *)lhs)->getSubExp1();
                    typeExp = ((Assignment *)s)->getType();
                }
            } else {
                // Assume an implicit reference
                addrExp = ((ImpRefStatement *)s)->getAddressExp();
                if (addrExp->isTypedExp() && ((TypedExp *)addrExp)->getType()->resolvesToPointer())
                    addrExp = ((Unary *)addrExp)->getSubExp1();
                typeExp = ((ImpRefStatement *)s)->getType();
                // typeExp should be a pointer expression, or a union of pointer types
                if (typeExp->resolvesToUnion())
                    typeExp = typeExp->asUnion()->dereferenceUnion();
                else {
                    assert(typeExp->resolvesToPointer());
                    typeExp = typeExp->asPointer()->getPointsTo();
                }
            }
            if (addrExp && signature->isAddrOfStackLocal(_prog, addrExp)) {
                int addr = 0;
                if (addrExp->getArity() == 2 && signature->isOpCompatStackLocal(addrExp->getOper())) {
                    Const *K = (Const *)((Binary *)addrExp)->getSubExp2();
                    if (K->isConst()) {
                        addr = K->getInt();
                        if (addrExp->getOper() == opMinus)
                            addr = -addr;
                    }
                }
                SharedType ty = ((TypingStatement *)s)->getType();
                LOG << "in proc " << getName() << " adding addrExp " << addrExp << "with type " << *ty << " to local table\n";
                Exp * loc_mem = Location::memOf(addrExp);
                localTable.addItem(ADDRESS::g(addr), lookupSym(*loc_mem, ty), typeExp);
                delete loc_mem;
            }
        }
    }

    debugPrintAll("after application of dfa type analysis");

    Boomerang::get()->alertDecompileDebugPoint(this, "after dfa type analysis");
}

// This is the core of the data-flow-based type analysis algorithm: implementing the meet operator.
// In classic lattice-based terms, the TOP type is void; there is no BOTTOM type since we handle overconstraints with
// unions.
// Consider various pieces of knowledge about the types. There could be:
// a) void: no information. Void meet x = x.
// b) size only: find a size large enough to contain the two types.
// c) broad type only, e.g. floating point
// d) signedness, no size
// e) size, no signedness
// f) broad type, size, and (for integer broad type), signedness

// ch set true if any change

SharedType VoidType::meetWith(SharedType other, bool &ch, bool /*bHighestPtr*/) const {
    // void meet x = x
    ch |= !other->resolvesToVoid();
    return other->clone();
}

SharedType FuncType::meetWith(SharedType other, bool &ch, bool bHighestPtr) const {
    if (other->resolvesToVoid())
        return ((FuncType *)this)->shared_from_this();
    // NOTE: at present, compares names as well as types and num parameters
    if (*this == *other)
        return ((FuncType *)this)->shared_from_this();
    return createUnion(other, ch, bHighestPtr);
}

SharedType IntegerType::meetWith(SharedType other, bool &ch, bool bHighestPtr) const {
    if (other->resolvesToVoid())
        return ((IntegerType *)this)->shared_from_this();
    if (other->resolvesToInteger()) {
        std::shared_ptr<IntegerType> otherInt = other->as<IntegerType>();
        // Signedness
        int oldSignedness = signedness;
        if (otherInt->signedness > 0)
            signedness++;
        else if (otherInt->signedness < 0)
            signedness--;
        ch |= ((signedness > 0) != (oldSignedness > 0)); // Changed from signed to not necessarily signed
        ch |= ((signedness < 0) != (oldSignedness < 0)); // Changed from unsigned to not necessarily unsigned
        // Size. Assume 0 indicates unknown size
        unsigned oldSize = size;
        size = std::max(size, otherInt->size);
        ch |= (size != oldSize);
        return ((IntegerType *)this)->shared_from_this();
    }
    if (other->resolvesToSize()) {
        std::shared_ptr<SizeType> other_sz=other->as<SizeType>();
        if (size == 0) { // Doubt this will ever happen
            size = other_sz->getSize();
            return ((IntegerType *)this)->shared_from_this();
        }
        if (size == other_sz->getSize())
            return ((IntegerType *)this)->shared_from_this();
        LOG << "integer size " << size << " meet with SizeType size " << other->as<SizeType>()->getSize() << "!\n";
        unsigned oldSize = size;
        size = std::max(size, other_sz->getSize());
        ch = size != oldSize;
        return ((IntegerType *)this)->shared_from_this();
    }
    return createUnion(other, ch, bHighestPtr);
}

SharedType FloatType::meetWith(SharedType other, bool &ch, bool bHighestPtr) const {
    if (other->resolvesToVoid())
        return ((FloatType *)this)->shared_from_this();
    if (other->resolvesToFloat()) {
        std::shared_ptr<FloatType> otherFlt(other->as<FloatType>());
        size_t oldSize = size;
        size = std::max(size, otherFlt->size);
        ch |= size != oldSize;
        return ((FloatType *)this)->shared_from_this();
    }
    if (other->resolvesToSize()) {
        size_t otherSize = other->getSize();
        ch |= size != otherSize;
        size = std::max(size, otherSize);
        return ((FloatType *)this)->shared_from_this();
    }
    return createUnion(other, ch, bHighestPtr);
}

SharedType BooleanType::meetWith(SharedType other, bool &ch, bool bHighestPtr) const {
    if (other->resolvesToVoid() || other->resolvesToBoolean())
        return ((BooleanType *)this)->shared_from_this();
    return createUnion(other, ch, bHighestPtr);
}

SharedType CharType::meetWith(SharedType other, bool &ch, bool bHighestPtr) const {
    if (other->resolvesToVoid() || other->resolvesToChar())
        return ((CharType *)this)->shared_from_this();
    // Also allow char to merge with integer
    if (other->resolvesToInteger()) {
        ch = true;
        return other->clone();
    }
    if (other->resolvesToSize() && other->as<SizeType>()->getSize() == 8)
        return ((CharType *)this)->shared_from_this();
    return createUnion(other, ch, bHighestPtr);
}

SharedType PointerType::meetWith(SharedType other, bool &ch, bool bHighestPtr) const {
    if (other->resolvesToVoid())
        return ((PointerType *)this)->shared_from_this();
    if (other->resolvesToSize() && other->asSize()->getSize() == STD_SIZE)
        return ((PointerType *)this)->shared_from_this();
    if (other->resolvesToPointer()) {
        auto otherPtr = other->asPointer();
        if (pointsToAlpha() && !otherPtr->pointsToAlpha()) {
            ch = true;
            // Can't point to self; impossible to compare, print, etc
            if (otherPtr->getPointsTo() == shared_from_this())
                return VoidType::get(); // TODO: pointer to void at least ?
            points_to = otherPtr->getPointsTo();
            return ((PointerType *)this)->shared_from_this();
        } else {
            // We have a meeting of two pointers.
            SharedType thisBase = points_to;
            SharedType otherBase = otherPtr->points_to;
            if (bHighestPtr) {
                // We want the greatest type of thisBase and otherBase
                if (thisBase->isSubTypeOrEqual(otherBase))
                    return other->clone();
                if (otherBase->isSubTypeOrEqual(thisBase))
                    return ((PointerType *)this)->shared_from_this();
                // There may be another type that is a superset of this and other; for now return void*
                return PointerType::get(VoidType::get());
            }
            // See if the base types will meet
            if (otherBase->resolvesToPointer()) {
                if (thisBase->resolvesToPointer() && thisBase->asPointer()->getPointsTo() == thisBase)
                    LOG_STREAM() << "HACK! BAD POINTER 1\n";
                if (otherBase->resolvesToPointer() && otherBase->asPointer()->getPointsTo() == otherBase)
                    LOG_STREAM() << "HACK! BAD POINTER 2\n";
                if (thisBase == otherBase) // Note: compare pointers
                    return ((PointerType *)this)->shared_from_this(); // Crude attempt to prevent stack overflow
                if (*thisBase == *otherBase)
                    return ((PointerType *)this)->shared_from_this();
                if (pointerDepth() == otherPtr->pointerDepth()) {
                    SharedType fType = getFinalPointsTo();
                    if (fType->resolvesToVoid())
                        return other->clone();
                    SharedType ofType = otherPtr->getFinalPointsTo();
                    if (ofType->resolvesToVoid())
                        return ((PointerType *)this)->shared_from_this();
                    if (*fType == *ofType)
                        return ((PointerType *)this)->shared_from_this();
                }
            }
            if (thisBase->isCompatibleWith(*otherBase)) {
                points_to = points_to->meetWith(otherBase, ch, bHighestPtr);
                return ((PointerType *)this)->shared_from_this();
            }
            // The bases did not meet successfully. Union the pointers.
            return createUnion(other, ch, bHighestPtr);
        }
        return ((PointerType *)this)->shared_from_this();
    }
    // Would be good to understand class hierarchys, so we know if a* is the same as b* when b is a subclass of a
    return createUnion(other, ch, bHighestPtr);
}

SharedType ArrayType::meetWith(SharedType other, bool &ch, bool bHighestPtr) const {
    if (other->resolvesToVoid())
        return ((ArrayType *)this)->shared_from_this();
    if (other->resolvesToArray()) {
        auto otherArr = other->asArray();
        SharedType newBase = BaseType->clone()->meetWith(otherArr->BaseType, ch, bHighestPtr);
        if (*newBase != *BaseType) {
            ch = true;
            Length = convertLength(newBase);
            BaseType = newBase; // No: call setBaseType to adjust length
        }
        if (other->asArray()->getLength() < getLength()) {
            Length = other->asArray()->getLength();
        }
        return std::const_pointer_cast<Type>(this->shared_from_this());
    }
    if (*BaseType == *other)
        return ((ArrayType *)this)->shared_from_this();
    /*
    * checks if 'other' is compatible with the ArrayType, if it is
    * checks if other's 'completeness' is less then current BaseType, if it is, unchanged type is returned.
    * checks if sizes of BaseType and other match, if they do, checks if other is less complete ( SizeType vs NonSize type ),
        if that happens unchanged type is returned
    * then it clones the BaseType and tries to 'meetWith' with other, if this results in unchanged type, unchanged type is returned
    * otherwise a new ArrayType is returned, with it's size recalculated based on new BaseType
    */
    if(isCompatible(*other,false)) { // compatible with all ?
        size_t bitsize = BaseType->getSize();
        size_t new_size = other->getSize();
        if(BaseType->isComplete() && !other->isComplete()) {
            // complete types win
            return std::const_pointer_cast<Type>(this->shared_from_this());
        }
        if(bitsize==new_size) {
            //same size, prefer Int/Float over SizeType
            if(!BaseType->isSize() && other->isSize()) {
                return std::const_pointer_cast<Type>(this->shared_from_this());
            }

        }
        auto bt = BaseType->clone();
        bool base_changed;
        auto res = bt->meetWith(other,base_changed);
        if(res==bt)
            return std::const_pointer_cast<Type>(this->shared_from_this());
        size_t new_length = Length;
        if(Length!=NO_BOUND) {
            new_length = (Length * bitsize ) / new_size;
        }
        return ArrayType::get(res,new_length);
    }
    // Needs work?
    return createUnion(other, ch, bHighestPtr);
}

SharedType NamedType::meetWith(SharedType other, bool &ch, bool bHighestPtr) const {
    SharedType rt = resolvesTo();
    if (rt) {
        SharedType ret = rt->meetWith(other, ch, bHighestPtr);
        if (ret == rt) // Retain the named type, much better than some compound type
            return ((NamedType *)this)->shared_from_this();
        return ret;              // Otherwise, whatever the result is
    }
    if (other->resolvesToVoid())
        return ((NamedType *)this)->shared_from_this();
    if (*this == *other)
        return ((NamedType *)this)->shared_from_this();
    return createUnion(other, ch, bHighestPtr);
}

SharedType CompoundType::meetWith(SharedType other, bool &ch, bool bHighestPtr) const {
    if (other->resolvesToVoid())
        return ((CompoundType *)this)->shared_from_this();
    if (!other->resolvesToCompound()) {
        if (types[0]->isCompatibleWith(*other))
            // struct meet first element = struct
            return ((CompoundType *)this)->shared_from_this();
        return createUnion(other, ch, bHighestPtr);
    }
    auto otherCmp = other->asCompound();
    if (otherCmp->isSuperStructOf(((CompoundType *)this)->shared_from_this())) {
        // The other structure has a superset of my struct's offsets. Preserve the names etc of the bigger struct.
        ch = true;
        return other;
    }
    if (isSubStructOf(otherCmp)) {
        // This is a superstruct of other
        ch = true;
        return ((CompoundType *)this)->shared_from_this();
    }
    if (*this == *other)
        return ((CompoundType *)this)->shared_from_this();
    // Not compatible structs. Create a union of both complete structs.
    // NOTE: may be possible to take advantage of some overlaps of the two structures some day.
    return createUnion(other, ch, bHighestPtr);
}

#define PRINT_UNION 0 // Set to 1 to debug unions to stderr
#ifdef PRINT_UNION
unsigned unionCount = 0;
#endif

SharedType UnionType::meetWith(SharedType other, bool &ch, bool bHighestPtr) const {
    if (other->resolvesToVoid())
        return ((UnionType *)this)->shared_from_this();
    if (other->resolvesToUnion()) {
        if (this == other.get())       // Note: pointer comparison
            return ((UnionType *)this)->shared_from_this(); // Avoid infinite recursion

        auto otherUnion = other->as<UnionType>();
        // Always return this, never other, (even if other is larger than this) because otherwise iterators can become
        // invalid below
        // TODO: verify the below, before the return was done after single meetWith on first file of other union
        for (UnionElement it : otherUnion->li) {
            meetWith(it.type, ch, bHighestPtr);
        }
        return ((UnionType *)this)->shared_from_this();
    }

    // Other is a non union type
    if (other->resolvesToPointer() && other->asPointer()->getPointsTo().get() == this) {
        LOG << "WARNING! attempt to union " << getCtype() << " with pointer to self!\n";
        return ((UnionType *)this)->shared_from_this();
    }

//    int subtypes_count = 0;
//    for (it = li.begin(); it != li.end(); ++it) {
//        Type &v(*it->type);
//        if(v.isCompound()) {
//            subtypes_count += ((CompoundType &)v).getNumTypes();
//        }
//        else if(v.isUnion()) {
//            subtypes_count += ((UnionType &)v).getNumTypes();
//        }
//        else
//            subtypes_count+=1;
//    }
//    if(subtypes_count>9) {
//        qDebug() << getCtype();
//        qDebug() << other->getCtype();
//        qDebug() << "*****";
//    }

    // Match 'other' agains all fields of 'this' UnionType
    // if a field is found that requires no change to 'meet', this type is returned unchanged
    // if a new meetWith result is 'better' given simplistic type description length heuristic measure
    // then the meetWith result, and this types field iterator are stored.
    int best_meet_quality=INT_MAX;
    SharedType best_so_far;
    UnionEntrySet::iterator location_of_meet=li.end();

    for (auto it = li.begin(); it != li.end(); ++it) {
        Type &v(*it->type);
        if (!v.isCompatibleWith(*other))
            continue;
        SharedType curr = v.clone();
        auto meet_res = curr->meetWith(other, ch, bHighestPtr);
        if(!ch) { // no change necessary for meet, perfect
//            qDebug() << getCtype();
            return ((UnionType *)this)->shared_from_this();
        }
        int quality = meet_res->getCtype().size();
        if(quality<best_meet_quality) {
//            qDebug() << "Found better match:" << meet_res->getCtype();
            best_so_far = meet_res;
            best_meet_quality = quality;
            location_of_meet = it;
        }
    }
    if(best_meet_quality!=INT_MAX) {
        UnionElement ne = *location_of_meet;
        ne.type = best_so_far;
        li.erase(location_of_meet);
        li.insert(ne);
//        qDebug() << getCtype();
        return ((UnionType *)this)->shared_from_this();
    }

    // Other is not compatible with any of my component types. Add a new type
#if PRINT_UNION                                   // Set above
    if (unionCount == 999)                        // Adjust the count to catch the one you want
        LOG_STREAM() << "createUnion breakpokint\n"; // Note: you need two breakpoints (also in Type::createUnion)
    LOG_STREAM() << "  " << ++unionCount << " Created union from " << getCtype() << " and " << other->getCtype();
#endif
    ((UnionType *)this)->addType(other->clone(), QString("x%1").arg(++nextUnionNumber));
#if PRINT_UNION
    LOG_STREAM() << ", result is " << getCtype() << "\n";
#endif
    ch = true;
    return ((UnionType *)this)->shared_from_this();
}

SharedType SizeType::meetWith(SharedType other, bool &ch, bool bHighestPtr) const {
    if (other->resolvesToVoid())
        return ((SizeType *)this)->shared_from_this();
    if (other->resolvesToSize()) {
        if (other->asSize()->size != size) {
            LOG << "size " << size << " meet with size " << other->asSize()->size << "!\n";
            unsigned oldSize = size;
            size = std::max(size, other->asSize()->size);
            ch = size != oldSize;
        }
        return ((SizeType *)this)->shared_from_this();
    }
    ch = true;
    if (other->resolvesToInteger() || other->resolvesToFloat() || other->resolvesToPointer()) {
        if (other->getSize() == 0) {
            other->setSize(size);
            return other->clone();
        }
        if (other->getSize() != size)
            LOG << "WARNING: size " << size << " meet with " << other->getCtype() << "; allowing temporarily\n";
        return other->clone();
    }
    return createUnion(other, ch, bHighestPtr);
}

SharedType UpperType::meetWith(SharedType other, bool &ch, bool bHighestPtr) const {
    if (other->resolvesToVoid())
        return ((UpperType *)this)->shared_from_this();
    if (other->resolvesToUpper()) {
        auto otherUpp = other->asUpper();
        auto newBase = base_type->clone()->meetWith(otherUpp->base_type, ch, bHighestPtr);
        if (*newBase != *base_type) {
            ch = true;
            base_type = newBase;
        }
        return ((UpperType *)this)->shared_from_this();
    }
    // Needs work?
    return createUnion(other, ch, bHighestPtr);
}

SharedType LowerType::meetWith(SharedType other, bool &ch, bool bHighestPtr) const {
    if (other->resolvesToVoid())
        return ((LowerType *)this)->shared_from_this();
    if (other->resolvesToUpper()) {
        std::shared_ptr<LowerType> otherLow = other->asLower();
        SharedType newBase = base_type->clone()->meetWith(otherLow->base_type, ch, bHighestPtr);
        if (*newBase != *base_type) {
            ch = true;
            base_type = newBase;
        }
        return ((LowerType *)this)->shared_from_this();
    }
    // Needs work?
    return createUnion(other, ch, bHighestPtr);
}

SharedType Instruction::meetWithFor(SharedType ty, Exp *e, bool &ch) {
    bool thisCh = false;
    SharedType typeFor = getTypeFor(e);
    assert(typeFor);
    SharedType newType = typeFor->meetWith(ty, thisCh);
    if (thisCh) {
        ch = true;
        setTypeFor(e, newType->clone());
    }
    return newType;
}

SharedType Type::createUnion(SharedType other, bool &ch, bool bHighestPtr /* = false */) const {

    assert(!resolvesToUnion()); // `this' should not be a UnionType
    if (other->resolvesToUnion()) // Put all the hard union logic in one place
        return other->meetWith(((Type *)this)->shared_from_this(), ch, bHighestPtr)->clone();
    // Check for anytype meet compound with anytype as first element
    if (other->resolvesToCompound()) {
        auto otherComp = other->asCompound();
        SharedType firstType = otherComp->getType((unsigned)0);
        if (firstType->isCompatibleWith(*this))
            // struct meet first element = struct
            return other->clone();
    }
    // Check for anytype meet array of anytype
    if (other->resolvesToArray()) {
        auto otherArr = other->asArray();
        SharedType elemTy = otherArr->getBaseType();
        if (elemTy->isCompatibleWith(*this))
            // array meet element = array
            return other->clone();
    }

    char name[20];
#if PRINT_UNION
    if (unionCount == 999)                        // Adjust the count to catch the one you want
        LOG_STREAM() << "createUnion breakpokint\n"; // Note: you need two breakpoints (also in UnionType::meetWith)
#endif
    sprintf(name, "x%d", ++nextUnionNumber);
    auto u = std::make_shared<UnionType>();
    u->addType(this->clone(), name);
    sprintf(name, "x%d", ++nextUnionNumber);
    u->addType(other->clone(), name);
    ch = true;
#if PRINT_UNION
    LOG_STREAM() << "  " << ++unionCount << " Created union from " << getCtype() << " and " << other->getCtype()
              << ", result is " << u->getCtype() << "\n";
#endif
    return u;
}

void CallStatement::dfaTypeAnalysis(bool &ch) {
    // Iterate through the arguments
    StatementList::iterator aa;
    int n = 0;
    for (aa = arguments.begin(); aa != arguments.end(); ++aa, ++n) {
        if (procDest && !procDest->getSignature()->getParamBoundMax(n).isNull() && ((Assign *)*aa)->getRight()->isIntConst()) {
            Assign *a = (Assign *)*aa;
            QString boundmax = procDest->getSignature()->getParamBoundMax(n);
            assert(a->getType()->resolvesToInteger());
            StatementList::iterator aat;
            int nt = 0;
            for (aat = arguments.begin(); aat != arguments.end(); ++aat, ++nt)
                if (boundmax == procDest->getSignature()->getParamName(nt)) {
                    SharedType tyt = ((Assign *)*aat)->getType();
                    if (tyt->resolvesToPointer() && tyt->asPointer()->getPointsTo()->resolvesToArray() &&
                        tyt->asPointer()->getPointsTo()->asArray()->isUnbounded())
                        tyt->asPointer()->getPointsTo()->asArray()->setLength(((Const *)a->getRight())->getInt());
                    break;
                }
        }
        // The below will ascend type, meet type with that of arg, and descend type. Note that the type of the assign
        // will already be that of the signature, if this is a library call, from updateArguments()
        ((Assign *)*aa)->dfaTypeAnalysis(ch);
    }
    // The destination is a pointer to a function with this function's signature (if any)
    if (pDest) {
        if (signature)
            pDest->descendType(FuncType::get(signature), ch, this);
        else if (procDest)
            pDest->descendType(FuncType::get(procDest->getSignature()), ch, this);
    }
}

void ReturnStatement::dfaTypeAnalysis(bool &ch) {
    StatementList::iterator mm, rr;
    for (mm = modifieds.begin(); mm != modifieds.end(); ++mm) {
        ((Assign *)*mm)->dfaTypeAnalysis(ch);
    }
    for (rr = returns.begin(); rr != returns.end(); ++rr) {
        ((Assign *)*rr)->dfaTypeAnalysis(ch);
    }
}

// For x0 := phi(x1, x2, ...) want
// Tx0 := Tx0 meet (Tx1 meet Tx2 meet ...)
// Tx1 := Tx1 meet Tx0
// Tx2 := Tx2 meet Tx0
// ...
void PhiAssign::dfaTypeAnalysis(bool &ch) {
    iterator it = DefVec.begin();
    while (it->second.e == nullptr && it != DefVec.end())
        ++it;
    assert(it != DefVec.end());
    SharedType meetOfArgs = it->second.def()->getTypeFor(lhs);
    for (++it; it != DefVec.end(); ++it) {
        PhiInfo &phinf(it->second);
        if (phinf.e == nullptr)
            continue;
        assert(phinf.def());
        SharedType typeOfDef = phinf.def()->getTypeFor(phinf.e);
        meetOfArgs = meetOfArgs->meetWith(typeOfDef, ch);
    }
    type = type->meetWith(meetOfArgs, ch);
    for (it = DefVec.begin(); it != DefVec.end(); ++it) {
        if (it->second.e == nullptr)
            continue;
        it->second.def()->meetWithFor(type, it->second.e, ch);
    }
    Assignment::dfaTypeAnalysis(ch); // Handle the LHS
}

void Assign::dfaTypeAnalysis(bool &ch) {
    SharedType tr = rhs->ascendType();
    type = type->meetWith(tr, ch, true); // Note: bHighestPtr is set true, since the lhs could have a greater type
    // (more possibilities) than the rhs. Example: pEmployee = pManager.
    rhs->descendType(type, ch, this); // This will effect rhs = rhs MEET lhs
    Assignment::dfaTypeAnalysis(ch);  // Handle the LHS wrt m[] operands
}

void Assignment::dfaTypeAnalysis(bool &ch) {
    Signature *sig = proc->getSignature();
    // Don't do this for the common case of an ordinary local, since it generates hundreds of implicit references,
    // without any new type information
    if (lhs->isMemOf() && !sig->isStackLocal(proc->getProg(), lhs)) {
        Exp *addr = lhs->getSubExp1();
        // Meet the assignment type with *(type of the address)
        SharedType addrType = addr->ascendType();
        SharedType memofType;
        if (addrType->resolvesToPointer())
            memofType = addrType->asPointer()->getPointsTo();
        else
            memofType = VoidType::get();
        type = type->meetWith(memofType, ch);
        // Push down the fact that the memof operand is a pointer to the assignment type
        addrType = PointerType::get(type);
        addr->descendType(addrType, ch, this);
    }
}

void BranchStatement::dfaTypeAnalysis(bool &ch) {
    if (pCond)
        pCond->descendType(BooleanType::get(), ch, this);
    // Not fully implemented yet?
}
//! Data flow based type analysis
void ImplicitAssign::dfaTypeAnalysis(bool &ch) { Assignment::dfaTypeAnalysis(ch); }

void BoolAssign::dfaTypeAnalysis(bool &ch) {
    // Not properly implemented yet
    Assignment::dfaTypeAnalysis(ch);
}

// Special operators for handling addition and subtraction in a data flow based type analysis
//                    ta=
//  tb=       alpha*     int      pi
//  beta*     bottom    void*    void*
//  int        void*     int      pi
//  pi         void*     pi       pi
SharedType sigmaSum(SharedType ta, SharedType tb) {
    bool ch;
    if (ta->resolvesToPointer()) {
        if (tb->resolvesToPointer())
            return ta->createUnion(tb, ch);
        return PointerType::get(VoidType::get());
    }
    if (ta->resolvesToInteger()) {
        if (tb->resolvesToPointer())
            return PointerType::get(VoidType::get());
        return tb->clone();
    }
    if (tb->resolvesToPointer())
        return PointerType::get(VoidType::get());
    return ta->clone();
}

//                    tc=
//  to=        beta*    int        pi
// alpha*    int        bottom    int
// int        void*    int        pi
// pi        pi        pi        pi
SharedType sigmaAddend(SharedType tc, SharedType to) {
    bool ch;
    if (tc->resolvesToPointer()) {
        if (to->resolvesToPointer())
            return IntegerType::get(STD_SIZE, 0);
        if (to->resolvesToInteger())
            return PointerType::get(VoidType::get());
        return to->clone();
    }
    if (tc->resolvesToInteger()) {
        if (to->resolvesToPointer())
            return tc->createUnion(to, ch);
        return to->clone();
    }
    if (to->resolvesToPointer())
        return IntegerType::get(STD_SIZE, 0);
    return tc->clone();
}

//                    tc=
//  tb=        beta*    int        pi
// alpha*    bottom    void*    void*
// int        void*    int        pi
// pi        void*    int        pi
SharedType deltaMinuend(SharedType tc, SharedType tb) {
    bool ch;
    if (tc->resolvesToPointer()) {
        if (tb->resolvesToPointer())
            return tc->createUnion(tb, ch);
        return PointerType::get(VoidType::get());
    }
    if (tc->resolvesToInteger()) {
        if (tb->resolvesToPointer())
            return PointerType::get(VoidType::get());
        return tc->clone();
    }
    if (tb->resolvesToPointer())
        return PointerType::get(VoidType::get());
    return tc->clone();
}

//                    tc=
//  ta=        beta*    int        pi
// alpha*    int        void*    pi
// int        bottom    int        int
// pi        int        pi        pi
SharedType deltaSubtrahend(SharedType tc, SharedType ta) {
    bool ch;
    if (tc->resolvesToPointer()) {
        if (ta->resolvesToPointer())
            return IntegerType::get(STD_SIZE, 0);
        if (ta->resolvesToInteger())
            return tc->createUnion(ta, ch);
        return IntegerType::get(STD_SIZE, 0);
    }
    if (tc->resolvesToInteger())
        if (ta->resolvesToPointer())
            return PointerType::get(VoidType::get());
    return ta->clone();
    if (ta->resolvesToPointer())
        return tc->clone();
    return ta->clone();
}

//                    ta=
//  tb=        alpha*    int        pi
// beta*    int        bottom    int
// int        void*    int        pi
// pi        pi        int        pi
SharedType deltaDifference(SharedType ta, SharedType tb) {
    bool ch;
    if (ta->resolvesToPointer()) {
        if (tb->resolvesToPointer())
            return IntegerType::get(STD_SIZE, 0);
        if (tb->resolvesToInteger())
            return PointerType::get(VoidType::get());
        return tb->clone();
    }
    if (ta->resolvesToInteger()) {
        if (tb->resolvesToPointer())
            return ta->createUnion(tb, ch);
        return IntegerType::get(STD_SIZE, 0);
    }
    if (tb->resolvesToPointer())
        return IntegerType::get(STD_SIZE, 0);
    return ta->clone();
}

//    //    //    //    //    //    //    //    //    //    //
//                                        //
//    ascendType: draw type information    //
//        up the expression tree            //
//                                        //
//    //    //    //    //    //    //    //    //    //    //

SharedType Binary::ascendType() {
    if (op == opFlagCall)
        return VoidType::get();
    SharedType ta = subExp1->ascendType();
    SharedType tb = subExp2->ascendType();
    switch (op) {
    case opPlus:
        return sigmaSum(ta, tb);
    // Do I need to check here for Array* promotion? I think checking in descendType is enough
    case opMinus:
        return deltaDifference(ta, tb);
    case opMult:
    case opDiv:
        return IntegerType::get(ta->getSize(), -1);
    case opMults:
    case opDivs:
    case opShiftRA:
        return IntegerType::get(ta->getSize(), +1);
    case opBitAnd:
    case opBitOr:
    case opBitXor:
    case opShiftR:
    case opShiftL:
        return IntegerType::get(ta->getSize(), 0);
    case opLess:
    case opGtr:
    case opLessEq:
    case opGtrEq:
    case opLessUns:
    case opGtrUns:
    case opLessEqUns:
    case opGtrEqUns:
        return BooleanType::get();
    default:
        // Many more cases to implement
        return VoidType::get();
    }
}

// Constants and subscripted locations are at the leaves of the expression tree. Just return their stored types.
SharedType RefExp::ascendType() {
    if (def == nullptr) {
        LOG_STREAM() << "Warning! Null reference in " << this << "\n";
        return VoidType::get();
    }
    return def->getTypeFor(subExp1);
}
SharedType Const::ascendType() {
    if (type->resolvesToVoid()) {
        switch (op) {
        case opIntConst:
// could be anything, Boolean, Character, we could be bit fiddling pointers for all we know - trentw
#if 0
                if (u.i != 0 && (u.i < 0x1000 && u.i > -0x100))
                    // Assume that small nonzero integer constants are of integer type (can't be pointers)
                    // But note that you can't say anything about sign; these are bit patterns, not HLL constants
                    // (e.g. all ones could be signed -1 or unsigned 0xFFFFFFFF)
                    type = IntegerType::get(STD_SIZE, 0);
#endif
            break;
        case opLongConst:
            type = IntegerType::get(STD_SIZE * 2, 0);
            break;
        case opFltConst:
            type = FloatType::get(64);
            break;
        case opStrConst:
            type = PointerType::get(CharType::get());
            break;
        case opFuncConst:
            type = FuncType::get(); // More needed here?
            break;
        default:
            assert(0); // Bad Const
        }
    }
    return type;
}
// Can also find various terminals at the leaves of an expression tree
SharedType Terminal::ascendType() {
    switch (op) {
    case opPC:
        return IntegerType::get(STD_SIZE, -1);
    case opCF:
    case opZF:
        return BooleanType::get();
    case opDefineAll:
        return VoidType::get();
    case opFlags:
        return IntegerType::get(STD_SIZE, -1);
    default:
        LOG_STREAM() << "ascendType() for terminal " << this << " not implemented!\n";
        return VoidType::get();
    }
}

SharedType Unary::ascendType() {
    SharedType ta = subExp1->ascendType();
    switch (op) {
    case opMemOf:
        if (ta->resolvesToPointer())
            return ta->asPointer()->getPointsTo();
        else
            return VoidType::get(); // NOT SURE! Really should be bottom
        break;
    case opAddrOf:
        return PointerType::get(ta);
        break;
    default:
        break;
    }
    return VoidType::get();
}

SharedType Ternary::ascendType() {
    switch (op) {
    case opFsize:
        return FloatType::get(((Const *)subExp2)->getInt());
    case opZfill:
    case opSgnEx: {
        int toSize = ((Const *)subExp2)->getInt();
        return Type::newIntegerLikeType(toSize, op == opZfill ? -1 : 1);
    }

    default:
        break;
    }
    return VoidType::get();
}

SharedType TypedExp::ascendType() { return type; }

//    //    //    //    //    //    //    //    //    //    //
//                                        //
//    descendType: push type information    //
//        down the expression tree        //
//                                        //
//    //    //    //    //    //    //    //    //    //    //

void Binary::descendType(SharedType parentType, bool &ch, Instruction *s) {
    if (op == opFlagCall)
        return;
    SharedType ta = subExp1->ascendType();
    SharedType tb = subExp2->ascendType();
    SharedType nt; // "New" type for certain operators
// The following is an idea of Mike's that is not yet implemented well. It is designed to handle the situation
// where the only reference to a local is where its address is taken. In the current implementation, it incorrectly
// triggers with every ordinary local reference, causing esp to appear used in the final program
#if 0
    Signature* sig = s->getProc()->getSignature();
    Prog* prog = s->getProc()->getProg();
    if (parentType->resolvesToPointer() && !parentType->asPointer()->getPointsTo()->resolvesToVoid() &&
            sig->isAddrOfStackLocal(prog, this)) {
        // This is the address of some local. What I used to do is to make an implicit assignment for the local, and
        // try to meet with the real assignment later. But this had some problems. Now, make an implicit *reference*
        // to the specified address; this should eventually meet with the main assignment(s).
        s->getProc()->setImplicitRef(s, this, parentType);
    }
#endif
    switch (op) {
    case opPlus:
        ta = ta->meetWith(sigmaAddend(parentType, tb), ch);
        subExp1->descendType(ta, ch, s);
        tb = tb->meetWith(sigmaAddend(parentType, ta), ch);
        subExp2->descendType(tb, ch, s);
        break;
    case opMinus:
        ta = ta->meetWith(deltaMinuend(parentType, tb), ch);
        subExp1->descendType(ta, ch, s);
        tb = tb->meetWith(deltaSubtrahend(parentType, ta), ch);
        subExp2->descendType(tb, ch, s);
        break;
    case opGtrUns:
    case opLessUns:
    case opGtrEqUns:
    case opLessEqUns: {
        nt = IntegerType::get(ta->getSize(), -1); // Used as unsigned
        ta = ta->meetWith(nt, ch);
        tb = tb->meetWith(nt, ch);
        subExp1->descendType(ta, ch, s);
        subExp2->descendType(tb, ch, s);
        break;
    }
    case opGtr:
    case opLess:
    case opGtrEq:
    case opLessEq: {
        nt = IntegerType::get(ta->getSize(), +1); // Used as signed
        ta = ta->meetWith(nt, ch);
        tb = tb->meetWith(nt, ch);
        subExp1->descendType(ta, ch, s);
        subExp2->descendType(tb, ch, s);
        break;
    }
    case opBitAnd:
    case opBitOr:
    case opBitXor:
    case opShiftR:
    case opShiftL:
    case opMults:
    case opDivs:
    case opShiftRA:
    case opMult:
    case opDiv: {
        int signedness;
        switch (op) {
        case opBitAnd:
        case opBitOr:
        case opBitXor:
        case opShiftR:
        case opShiftL:
            signedness = 0;
            break;
        case opMults:
        case opDivs:
        case opShiftRA:
            signedness = 1;
            break;
        case opMult:
        case opDiv:
            signedness = -1;
            break;
        default:
            signedness = 0;
            break; // Unknown signedness
        }

        int parentSize = parentType->getSize();
        ta = ta->meetWith(IntegerType::get(parentSize, signedness), ch);
        subExp1->descendType(ta, ch, s);
        if (op == opShiftL || op == opShiftR || op == opShiftRA)
            // These operators are not symmetric; doesn't force a signedness on the second operand
            // FIXME: should there be a gentle bias twowards unsigned? Generally, you can't shift by negative
            // amounts.
            signedness = 0;
        tb = tb->meetWith(IntegerType::get(parentSize, signedness), ch);
        subExp2->descendType(tb, ch, s);
        break;
    }
    default:
        // Many more cases to implement
        break;
    }
}

void RefExp::descendType(SharedType parentType, bool &ch, Instruction *s) {
    SharedType newType = def->meetWithFor(parentType, subExp1, ch);
    // In case subExp1 is a m[...]
    subExp1->descendType(newType, ch, s);
}

void Const::descendType(SharedType parentType, bool &ch, Instruction */*s*/) {
    bool thisCh = false;
    type = type->meetWith(parentType, thisCh);
    ch |= thisCh;
    if (thisCh) {
        // May need to change the representation
        if (type->resolvesToFloat()) {
            if (op == opIntConst) {
                op = opFltConst;
                type = FloatType::get(64);
                float f = *(float *)&u.i;
                u.d = (double)f;
            } else if (op == opLongConst) {
                op = opFltConst;
                type = FloatType::get(64);
                double d = *(double *)&u.ll;
                u.d = d;
            }
        }
        // May be other cases
    }
}
// match m[l1{} + K] pattern
static bool match_l1_K(Exp *in,std::vector<Exp *> &matches) {
    if(!in->isMemOf())
        return false;
    Binary *as_bin = dynamic_cast<Binary *>(in->getSubExp1());
    if(!as_bin || as_bin->getOper()!=opPlus)
        return false;
    if(!as_bin->getSubExp2()->isIntConst())
        return false;
    if(!as_bin->getSubExp1()->isSubscript())
        return false;
    RefExp * refexp= static_cast<RefExp *>(as_bin->getSubExp1());
    if(!refexp->getSubExp1()->isLocation())
        return false;
    Location *loc = static_cast<Location *>(refexp->getSubExp1());
    matches.push_back(refexp);
    matches.push_back(as_bin->getSubExp2());
    return true;
}
void Unary::descendType(SharedType parentType, bool &ch, Instruction *s) {
    UserProc *owner_proc = s->getProc();
    Signature *sig = owner_proc != nullptr ? owner_proc->getSignature() : nullptr;
    Prog *prog = owner_proc->getProg();
    std::vector<Exp *> matches;
    Binary *as_bin = dynamic_cast<Binary *>(subExp1);
    switch (op) {
    case opMemOf:
    {
        // Check for m[x*K1 + K2]: array with base K2 and stride K1
        if (as_bin && as_bin->getOper() == opPlus && as_bin->getSubExp1()->getOper() == opMult &&
                as_bin->getSubExp2()->isIntConst() &&
                as_bin->getSubExp1()->getSubExp2()->isIntConst()) {
            Exp *leftOfPlus = as_bin->getSubExp1();
            // We would expect the stride to be the same size as the base type
            size_t stride = ((Const *)(leftOfPlus->getSubExp2()))->getInt();
            if (DEBUG_TA && stride * 8 != parentType->getSize())
                LOG << "type WARNING: apparent array reference at " << this << " has stride " << stride * 8
                    << " bits, but parent type " << parentType->getCtype() << " has size " << parentType->getSize()
                    << "\n";
            // The index is integer type
            Exp *x = ((Binary *)leftOfPlus)->getSubExp1();
            x->descendType(IntegerType::get(parentType->getSize(), 0), ch, s);
            // K2 is of type <array of parentType>
            Const *constK2 = (Const *)((Binary *)subExp1)->getSubExp2();
            ADDRESS intK2 = ADDRESS::g(constK2->getInt()); // TODO: use getAddr ?
            constK2->descendType(prog->makeArrayType(intK2, parentType), ch, s);
        } else if (match_l1_K(this,matches)) {
            // m[l1 + K]
            Location *l1 = (Location *)matches[0];
            SharedType l1Type = l1->ascendType();
            int K = ((Const *)matches[1])->getInt();
            if (l1Type->resolvesToPointer()) {
                // This is a struct reference m[ptr + K]; ptr points to the struct and K is an offset into it
                // First find out if we already have struct information
                SharedType st(l1Type->asPointer()->getPointsTo());
                if (st->resolvesToCompound()) {
                    auto ct = st->asCompound();
                    if (ct->isGeneric())
                        ct->updateGenericMember(K, parentType, ch);
                    else {
                        // would like to force a simplify here; I guess it will happen soon enough
                        ;
                    }
                } else {
                    // Need to create a generic stuct with a least one member at offset K
                    auto ct = CompoundType::get(true);
                    ct->updateGenericMember(K, parentType, ch);
                }
            } else {
                // K must be the pointer, so this is a global array
                // FIXME: finish this case
            }
            // FIXME: many other cases
        } else
            subExp1->descendType(PointerType::get(parentType), ch, s);
        break;
    }
    case opAddrOf:
        if (parentType->resolvesToPointer())
            subExp1->descendType(parentType->asPointer()->getPointsTo(), ch, s);
        break;
    case opGlobal: {
        Prog *prog = s->getProc()->getProg();
        QString name = ((Const *)subExp1)->getStr();
        SharedType ty = prog->getGlobalType(name);
        if (ty) {
            ty = ty->meetWith(parentType, ch);
            if(ch)
                prog->setGlobalType(name, ty);
        }
        break;
    }
    default:
        break;
    }
}

void Ternary::descendType(SharedType /*parentType*/, bool &ch, Instruction *s) {
    switch (op) {
    case opFsize:
        subExp3->descendType(FloatType::get(((Const *)subExp1)->getInt()), ch, s);
        break;
    case opZfill:
    case opSgnEx: {
        int fromSize = ((Const *)subExp1)->getInt();
        SharedType fromType;
        fromType = Type::newIntegerLikeType(fromSize, op == opZfill ? -1 : 1);
        subExp3->descendType(fromType, ch, s);
        break;
    }

    default:
        break;
    }
}

void TypedExp::descendType(SharedType /*parentType*/, bool &/*ch*/, Instruction */*s*/) {}

void Terminal::descendType(SharedType /*parentType*/, bool &/*ch*/, Instruction */*s*/) {}
//! Data flow based type analysis.
//! Meet the parameters with their current types.
//! \returns true if a change
bool Signature::dfaTypeAnalysis(Cfg *cfg) {
    bool ch = false;
    std::vector<Parameter *>::iterator it;
    for (Parameter * it : params) {
        // Parameters should be defined in an implicit assignment
        Instruction *def = cfg->findImplicitParamAssign(it);
        if (def) { // But sometimes they are not used, and hence have no implicit definition
            bool thisCh = false;
            def->meetWithFor(it->getType(), it->getExp(), thisCh);
            if (thisCh) {
                ch = true;
                if (DEBUG_TA)
                    LOG << "  sig caused change: " << it->getType()->getCtype() << " " << it->name() << "\n";
            }
        }
    }
    return ch;
}

// Note: to prevent infinite recursion, CompoundType, ArrayType, and UnionType implement this function as a delegation
// to isCompatible()
bool Type::isCompatibleWith(const Type &other, bool all /* = false */) const {
    if (other.resolvesToCompound() || other.resolvesToArray() || other.resolvesToUnion())
        return other.isCompatible(*this, all);
    return isCompatible(other, all);
}

bool VoidType::isCompatible(const Type &/*other*/, bool /*all*/) const {
    return true; // Void is compatible with any type
}

bool SizeType::isCompatible(const Type &other, bool /*all*/) const {
    if (other.resolvesToVoid())
        return true;
    size_t otherSize = other.getSize();
    if (other.resolvesToFunc())
        return false;
    // FIXME: why is there a test for size 0 here?
    // This is because some signatures leave us with 0-sized NamedType -> using GLEnum when it was not defined.
    if (otherSize == size)
        return true;
    if (other.resolvesToUnion())
        return other.isCompatibleWith(*this);
    if (other.resolvesToArray())
        return isCompatibleWith(*((const ArrayType &)other).getBaseType());
    // return false;
    // For now, size32 and double will be considered compatible (helps test/pentium/global2)
    return false;
}

bool IntegerType::isCompatible(const Type &other, bool /*all*/) const {
    if (other.resolvesToVoid())
        return true;
    if (other.resolvesToInteger())
        return true;
    if (other.resolvesToChar())
        return true;
    if (other.resolvesToUnion())
        return other.isCompatibleWith(*this);
    if (other.resolvesToSize() && ((const SizeType &)other).getSize() == size)
        return true;
    return false;
}

bool FloatType::isCompatible(const Type &other, bool /*all*/) const {
    if (other.resolvesToVoid())
        return true;
    if (other.resolvesToFloat())
        return true;
    if (other.resolvesToUnion())
        return other.isCompatibleWith(*this);
    if (other.resolvesToArray())
        return isCompatibleWith(*((const ArrayType &)other).getBaseType());
    if (other.resolvesToSize() && ((const SizeType &)other).getSize() == size)
        return true;
    return false;
}

bool CharType::isCompatible(const Type &other, bool /*all*/) const {
    if (other.resolvesToVoid())
        return true;
    if (other.resolvesToChar())
        return true;
    if (other.resolvesToInteger())
        return true;
    if (other.resolvesToSize() && ((const SizeType &)other).getSize() == 8)
        return true;
    if (other.resolvesToUnion())
        return other.isCompatibleWith(*this);
    if (other.resolvesToArray())
        return isCompatibleWith(*((const ArrayType &)other).getBaseType());
    return false;
}

bool BooleanType::isCompatible(const Type &other, bool /*all*/) const {
    if (other.resolvesToVoid())
        return true;
    if (other.resolvesToBoolean())
        return true;
    if (other.resolvesToUnion())
        return other.isCompatibleWith(*this);
    if (other.resolvesToSize() && ((const SizeType &)other).getSize() == 1)
        return true;
    return false;
}

bool FuncType::isCompatible(const Type &other, bool /*all*/) const {
    assert(signature);
    if (other.resolvesToVoid())
        return true;
    if (*this == other)
        return true; // MVE: should not compare names!
    if (other.resolvesToUnion())
        return other.isCompatibleWith(*this);
    if (other.resolvesToSize() && ((const SizeType &)other).getSize() == STD_SIZE)
        return true;
    if (other.resolvesToFunc()) {
        assert(other.asFunc()->signature);
        if (*other.asFunc()->signature == *signature)
            return true;
    }
    return false;
}

bool PointerType::isCompatible(const Type &other, bool /*all*/) const {
    if (other.resolvesToVoid())
        return true;
    if (other.resolvesToUnion())
        return other.isCompatibleWith(*this);
    if (other.resolvesToSize() && ((const SizeType &)other).getSize() == STD_SIZE)
        return true;
    if (!other.resolvesToPointer())
        return false;
    return points_to->isCompatibleWith(*other.asPointer()->points_to);
}

bool NamedType::isCompatible(const Type &other, bool /*all*/) const {
    if (other.isNamed() && name == ((const NamedType &)other).getName())
        return true;
    SharedType resTo = resolvesTo();
    if (resTo)
        return resolvesTo()->isCompatibleWith(other);
    if (other.resolvesToVoid())
        return true;
    return (*this == other);
}

bool ArrayType::isCompatible(const Type &other, bool all) const {
    if (other.resolvesToVoid())
        return true;
    if (other.resolvesToArray() && BaseType->isCompatibleWith(*other.asArray()->BaseType))
        return true;
    if (other.resolvesToUnion())
        return other.isCompatibleWith(*this);
    if (!all && BaseType->isCompatibleWith(other))
        return true; // An array of x is compatible with x
    return false;
}

bool UnionType::isCompatible(const Type &other, bool all) const {
    if (other.resolvesToVoid())
        return true;
    if (other.resolvesToUnion()) {
        if (this == &other) // Note: pointer comparison
            return true;   // Avoid infinite recursion
        const UnionType &otherUnion((const UnionType &)other);
        // Unions are compatible if one is a subset of the other
        if (li.size() < otherUnion.li.size()) {
            for (const UnionElement &e : li)
                if (!otherUnion.isCompatible(*e.type, all))
                    return false;
        } else {
            for (const UnionElement &e : otherUnion.li)
                if (!isCompatible(*e.type, all))
                    return false;
        }
        return true;
    }
    // Other is not a UnionType
    for (const UnionElement &e : li)
        if (other.isCompatibleWith(*e.type, all))
            return true;
    return false;
}

bool CompoundType::isCompatible(const Type &other, bool all) const {
    if (other.resolvesToVoid())
        return true;
    if (other.resolvesToUnion())
        return other.isCompatibleWith(*this);
    if (!other.resolvesToCompound())
        // Used to always return false here. But in fact, a struct is compatible with its first member (if all is false)
        return !all && types[0]->isCompatibleWith(other);
    auto otherComp = other.asCompound();
    int n = otherComp->getNumTypes();
    if (n != (int)types.size())
        return false; // Is a subcompound compatible with a supercompound?
    for (int i = 0; i < n; i++)
        if (!types[i]->isCompatibleWith(*otherComp->types[i]))
            return false;
    return true;
}

bool UpperType::isCompatible(const Type &other, bool /*all*/) const {
    if (other.resolvesToVoid())
        return true;
    if (other.resolvesToUpper() && base_type->isCompatibleWith(*other.asUpper()->base_type))
        return true;
    if (other.resolvesToUnion())
        return other.isCompatibleWith(*this);
    return false;
}

bool LowerType::isCompatible(const Type &other, bool /*all*/) const {
    if (other.resolvesToVoid())
        return true;
    if (other.resolvesToLower() && base_type->isCompatibleWith(*other.asLower()->base_type))
        return true;
    if (other.resolvesToUnion())
        return other.isCompatibleWith(*this);
    return false;
}

bool Type::isSubTypeOrEqual(SharedType other) {
    if (resolvesToVoid())
        return true;
    if (*this == *other)
        return true;
    if (this->resolvesToCompound() && other->resolvesToCompound())
        return this->asCompound()->isSubStructOf(other);
    // Not really sure here
    return false;
}

SharedType Type::dereference() {
    if (resolvesToPointer())
        return asPointer()->getPointsTo();
    if (resolvesToUnion())
        return asUnion()->dereferenceUnion();
    return VoidType::get(); // Can't dereference this type. Note: should probably be bottom
}

// Dereference this union. If it is a union of pointers, return a union of the dereferenced items. Else return VoidType
// (note: should probably be bottom)
SharedType UnionType::dereferenceUnion() {
    auto ret = UnionType::get();
    char name[20];
    UnionEntrySet::iterator it;
    for (it = li.begin(); it != li.end(); ++it) {
        SharedType elem = it->type->dereference();
        if (elem->resolvesToVoid())
            return elem; // Return void for the whole thing
        sprintf(name, "x%d", ++nextUnionNumber);
        ret->addType(elem->clone(), name);
    }
    return ret;
}
