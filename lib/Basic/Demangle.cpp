//===--- Demangle.cpp - Swift Name Demangling -----------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===---------------------------------------------------------------------===//
//
//  This file implements declaration name demangling in Swift.
//
//===---------------------------------------------------------------------===//

#include "swift/Basic/Demangle.h"
#include "swift/Strings.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Punycode.h"
#include "swift/Basic/UUID.h"
#include "llvm/ADT/StringRef.h"
#include <functional>
#include <ostream>
#include <sstream>
#include <vector>
#include <cstdlib>

using namespace swift;
using namespace Demangle;

[[noreturn]]
static void unreachable(const char *Message) {
  fprintf(stderr, "fatal error: %s\n", Message);
  std::abort();
}

namespace {
struct QuotedString {
  std::string Value;

  QuotedString(std::string Value) : Value(Value) {}
};
} // end unnamed namespace

std::ostream &operator<<(std::ostream &OS, const QuotedString &QS) {
  OS << '"';
  for (auto C : QS.Value) {
    switch (C) {
    case '\\': OS << "\\\\"; break;
    case '\t': OS << "\\t"; break;
    case '\n': OS << "\\n"; break;
    case '\r': OS << "\\r"; break;
    case '"': OS << "\\\""; break;
    case '\'': OS << '\''; break; // no need to escape these
    case '\0': OS << "\\0"; break;
    default:
      auto c = static_cast<unsigned char>(C);
      // Other ASCII control characters should get escaped.
      if (c < 0x20 || c == 0x7F) {
        static const char Hexdigit[] = {
          '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
          'A', 'B', 'C', 'D', 'E', 'F'
        };
        OS << "\\x" << Hexdigit[c >> 4] << Hexdigit[c & 0xF];
      } else {
        OS << c;
      }
      break;
    }
  }
  OS << '"';
  return OS;
}

namespace swift {
namespace Demangle {
struct NodeFactory {
  static NodePointer create(Node::Kind K) {
    return NodePointer(new Node(K));
  }
  static NodePointer create(Node::Kind K, Node::IndexType Index) {
    return NodePointer(new Node(K, Index));
  }
  static NodePointer create(Node::Kind K, llvm::StringRef Text) {
    return NodePointer(new Node(K, Text));
  }
  static NodePointer create(Node::Kind K, std::string &&Text) {
    return NodePointer(new Node(K, std::move(Text)));
  }
  template <size_t N>
  static NodePointer create(Node::Kind K, const char (&Text)[N]) {
    return NodePointer(new Node(K, llvm::StringRef(Text)));
  }
};
} // end namespace Demangle
} // end namespace swift

Node::~Node() {
  switch (NodePayloadKind) {
  case PayloadKind::None: return;
  case PayloadKind::Index: return;
  case PayloadKind::Text: TextPayload.~basic_string(); return;
  }
  unreachable("bad payload kind");
}

namespace {
  struct FindPtr {
    FindPtr(Node *v) : Target(v) {}
    bool operator()(NodePointer sp) const {
      return sp.get() == Target;
    }
  private:
    Node *Target;
  };

/// A class for printing to a std::string.
class DemanglerPrinter {
public:
  DemanglerPrinter() {}

  template <class T> DemanglerPrinter &operator<<(T &&Value) {
    Stream << std::forward<T>(Value);
    return *this;
  }

  DemanglerPrinter &operator<<(StringRef Value) {
    Stream.write(Value.data(), Value.size());
    return *this;
  }

  /// Destructively take the contents of this stream.
  std::string str() { return std::move(Stream.str()); }

private:
  std::stringstream Stream;
};
} // end anonymous namespace

static bool isStartOfIdentifier(char c) {
  if (c >= '0' && c <= '9')
    return true;
  return c == 'o';
}

static bool isStartOfNominalType(char c) {
  switch (c) {
  case 'C':
  case 'V':
  case 'O':
    return true;
  default:
    return false;
  }
}

static bool isStartOfEntity(char c) {
  switch (c) {
  case 'F':
  case 'I':
  case 'v':
  case 'P':
  case 's':
    return true;
  default:
    return isStartOfNominalType(c);
  }
}

static Node::Kind nominalTypeMarkerToNodeKind(char c) {
  if (c == 'C')
    return Node::Kind::Class;
  if (c == 'V')
    return Node::Kind::Structure;
  if (c == 'O')
    return Node::Kind::Enum;
  return Node::Kind::Identifier;
}

static std::string archetypeName(Node::IndexType i) {
  DemanglerPrinter name;
  do {
    name << (char)('A' + (i % 26));
    i /= 26;
  } while (i);
  return name.str();
}

namespace {
  enum class FunctionSigSpecializationParamInfoKind : uint8_t {
    ConstantPropFunction,
    ConstantPropGlobal,
    ConstantPropInteger,
    ConstantPropFloat,
    ConstantPropString,
    ClosureProp,
  };
} // end anonymous namespace

namespace {

/// A convenient class for parsing characters out of a string.
class NameSource {
  StringRef Text;
public:
  NameSource(StringRef text) : Text(text) {}

  /// Return whether there are at least len characters remaining.
  bool hasAtLeast(size_t len) { return (len <= Text.size()); }

  bool isEmpty() { return Text.empty(); }
  explicit operator bool() { return !isEmpty(); }

  /// Return the next character without claiming it.  Asserts that
  /// there is at least one remaining character.
  char peek() { return Text.front(); }

  /// Claim and return the next character.  Asserts that there is at
  /// least one remaining character.
  char next() {
    char c = peek();
    advanceOffset(1);
    return c;
  }

  /// Claim the next character if it exists and equals the given
  /// character.
  bool nextIf(char c) {
    if (isEmpty() || peek() != c) return false;
    advanceOffset(1);
    return true;
  }

  bool nextIfNot(char c) {
    if (isEmpty() || peek() == c) return false;
    advanceOffset(1);
    return true;
  }

  /// Claim the next few characters if they exactly match the given string.
  bool nextIf(StringRef str) {
    if (!Text.startswith(str)) return false;
    advanceOffset(str.size());
    return true;
  }

  /// Return the next len characters without claiming them.  Asserts
  /// that there are at least so many characters.
  StringRef slice(size_t len) { return Text.substr(0, len); }

  /// Return the current string ref without claiming any characters.
  StringRef str() { return Text; }

  /// Claim the next len characters.
  void advanceOffset(size_t len) {
    Text = Text.substr(len);
  }

  /// Claim and return all the rest of the characters.
  StringRef getString() {
    auto result = Text;
    advanceOffset(Text.size());
    return result;
  }
};

/// The main class for parsing a demangling tree out of a mangled string.
class Demangler {
  std::vector<NodePointer> Substitutions;
  std::vector<unsigned> ArchetypeCounts;
  unsigned ArchetypeCount = 0;
  NameSource Mangled;
  NodePointer RootNode;
public:  
  Demangler(llvm::StringRef mangled) : Mangled(mangled) {}

  /// Attempt to demangle the source string.  The root node will
  /// always be a Global.  Extra characters at the end will be
  /// tolerated (and included as a Suffix node as a child of the
  /// Global).
  ///
  /// \return true if the mangling succeeded
  bool demangle() {
    if (!Mangled.hasAtLeast(2))
      return failure();
    if (Mangled.slice(2) != "_T")
      return failure();

    // First demangle any specialization prefixes.
    if (Mangled.hasAtLeast(4) && Mangled.slice(4) == "_TTS") {
      do {
        Mangled.advanceOffset(4);
        auto attr = demangleSpecializedAttribute();
        if (!attr)
          return failure();
        appendNode(attr);
        // The Substitution header does not share state with the rest of the
        // mangling.
        Substitutions.clear();
        ArchetypeCounts.clear();
        ArchetypeCount = 0;
      } while (Mangled.hasAtLeast(4) && Mangled.slice(4) == "_TTS");

      // Then check that we have a global.
      if (!Mangled.hasAtLeast(2) || Mangled.slice(2) != "_T")
        return failure();
      Mangled.advanceOffset(2);

    } else if (Mangled.hasAtLeast(4) && Mangled.slice(4) == "_TTo") {
      Mangled.advanceOffset(4);
      appendNode(Node::Kind::ObjCAttribute);
    } else if (Mangled.hasAtLeast(4) && Mangled.slice(4) == "_TTO") {
      Mangled.advanceOffset(4);
      appendNode(Node::Kind::NonObjCAttribute);
    } else if (Mangled.hasAtLeast(4) && Mangled.slice(4) == "_TTD") {
      Mangled.advanceOffset(4);
      appendNode(Node::Kind::DynamicAttribute);
    } else {
      Mangled.advanceOffset(2);
    }
    
    NodePointer global = demangleGlobal();
    if (!global) return failure();
    appendNode(std::move(global));

    // Add a suffix node if there's anything left unmangled.
    if (!Mangled.isEmpty()) {
      appendNode(Node::Kind::Suffix, Mangled.getString());
    }

    return true;
  }

  bool demangleTypeName() {
    NodePointer global = demangleType();
    if (!global) return failure();
    appendNode(std::move(global));
    return true;
  }
  
  NodePointer getDemangled() { return RootNode; }
  
private:
  NodePointer getRootNode() {
    if (!RootNode) {
      RootNode = NodeFactory::create(Node::Kind::Global);
    }
    return RootNode;
  }

  NodePointer appendNode(NodePointer n) {
    return getRootNode()->addChild(std::move(n));
  }

  NodePointer appendNode(Node::Kind k, std::string &&t = "") {
    return appendNode(NodeFactory::create(k, std::move(t)));
  }

/// Try to demangle a child node of the given kind.  If that fails,
/// return; otherwise add it to the parent.
#define DEMANGLE_CHILD_OR_RETURN(PARENT, CHILD_KIND) do { \
    auto _node = demangle##CHILD_KIND();                  \
    if (!_node) return nullptr;                           \
    (PARENT)->addChild(std::move(_node));                 \
  } while (false)

/// Try to demangle a child node of the given kind.  If that fails,
/// return; otherwise add it to the parent.
#define DEMANGLE_CHILD_AS_NODE_OR_RETURN(PARENT, CHILD_KIND) do {  \
    auto _kind = demangle##CHILD_KIND();                           \
    if (_kind == CHILD_KIND::Unknown) return nullptr;              \
    (PARENT)->addChild(NodeFactory::create(Node::Kind::CHILD_KIND, \
                                           toString(_kind)));      \
  } while (false)

  enum class IsProtocol {
    yes = true, no = false
  };

  enum class IsVariadic {
    yes = true, no = false
  };

  enum class Directness {
    Unknown = 0, Direct, Indirect
  };

  StringRef toString(Directness d) {
    switch (d) {
    case Directness::Direct:
      return "direct";
    case Directness::Indirect:
      return "indirect";
    case Directness::Unknown:
      unreachable("shouldn't toString an unknown directness");
    }
    unreachable("bad directness");
  }

  bool failure() {
    RootNode = NodeFactory::create(Node::Kind::Failure);
    return false;
  }

  Directness demangleDirectness() {
    if (Mangled.nextIf('d'))
      return Directness::Direct;
    if (Mangled.nextIf('i'))
      return Directness::Indirect;
    return Directness::Unknown;
  }

  bool demangleNatural(Node::IndexType &num) {
    if (!Mangled)
      return false;
    char c = Mangled.next();
    if (c < '0' || c > '9')
      return false;
    num = (c - '0');
    while (true) {
      if (!Mangled) {
        return true;
      }
      c = Mangled.peek();
      if (c < '0' || c > '9') {
        return true;
      } else {
        num = (10 * num) + (c - '0');
      }
      Mangled.next();
    }
  }

  bool demangleBuiltinSize(Node::IndexType &num) {
    if (!demangleNatural(num))
      return false;
    if (Mangled.nextIf('_'))
      return true;
    return false;
  }

  enum class ValueWitnessKind {
    AllocateBuffer,
    AssignWithCopy,
    AssignWithTake,
    DeallocateBuffer,
    Destroy,
    DestroyBuffer,
    InitializeBufferWithCopyOfBuffer,
    InitializeBufferWithCopy,
    InitializeWithCopy,
    InitializeBufferWithTake,
    InitializeWithTake,
    ProjectBuffer,
    InitializeBufferWithTakeOfBuffer,
    DestroyArray,
    InitializeArrayWithCopy,
    InitializeArrayWithTakeFrontToBack,
    InitializeArrayWithTakeBackToFront,
    StoreExtraInhabitant,
    GetExtraInhabitantIndex,
    GetEnumTag,
    InplaceProjectEnumData,
    Unknown
  };

  StringRef toString(ValueWitnessKind k) {
    switch (k) {
    case ValueWitnessKind::AllocateBuffer:
      return "allocateBuffer";
    case ValueWitnessKind::AssignWithCopy:
      return "assignWithCopy";
    case ValueWitnessKind::AssignWithTake:
      return "assignWithTake";
    case ValueWitnessKind::DeallocateBuffer:
      return "deallocateBuffer";
    case ValueWitnessKind::Destroy:
      return "destroy";
    case ValueWitnessKind::DestroyBuffer:
      return "destroyBuffer";
    case ValueWitnessKind::InitializeBufferWithCopyOfBuffer:
      return "initializeBufferWithCopyOfBuffer";
    case ValueWitnessKind::InitializeBufferWithCopy:
      return "initializeBufferWithCopy";
    case ValueWitnessKind::InitializeWithCopy:
      return "initializeWithCopy";
    case ValueWitnessKind::InitializeBufferWithTake:
      return "initializeBufferWithTake";
    case ValueWitnessKind::InitializeWithTake:
      return "initializeWithTake";
    case ValueWitnessKind::ProjectBuffer:
      return "projectBuffer";
    case ValueWitnessKind::InitializeBufferWithTakeOfBuffer:
      return "initializeBufferWithTakeOfBuffer";
    case ValueWitnessKind::DestroyArray:
      return "destroyArray";
    case ValueWitnessKind::InitializeArrayWithCopy:
      return "initializeArrayWithCopy";
    case ValueWitnessKind::InitializeArrayWithTakeFrontToBack:
      return "initializeArrayWithTakeFrontToBack";
    case ValueWitnessKind::InitializeArrayWithTakeBackToFront:
      return "initializeArrayWithTakeBackToFront";
    case ValueWitnessKind::StoreExtraInhabitant:
      return "storeExtraInhabitant";
    case ValueWitnessKind::GetExtraInhabitantIndex:
      return "getExtraInhabitantIndex";
    case ValueWitnessKind::GetEnumTag:
      return "getEnumTag";
    case ValueWitnessKind::InplaceProjectEnumData:
      return "inplaceProjectEnumData";
    case ValueWitnessKind::Unknown:
      unreachable("stringifying the unknown value witness kind?");
    }
    unreachable("bad value witness kind");
  }

  ValueWitnessKind demangleValueWitnessKind() {
    if (!Mangled)
      return ValueWitnessKind::Unknown;
    char c1 = Mangled.next();
    if (!Mangled)
      return ValueWitnessKind::Unknown;
    char c2 = Mangled.next();
    if (c1 == 'a' && c2 == 'l')
      return ValueWitnessKind::AllocateBuffer;
    if (c1 == 'c' && c2 == 'a')
      return ValueWitnessKind::AssignWithCopy;
    if (c1 == 't' && c2 == 'a')
      return ValueWitnessKind::AssignWithTake;
    if (c1 == 'd' && c2 == 'e')
      return ValueWitnessKind::DeallocateBuffer;
    if (c1 == 'x' && c2 == 'x')
      return ValueWitnessKind::Destroy;
    if (c1 == 'X' && c2 == 'X')
      return ValueWitnessKind::DestroyBuffer;
    if (c1 == 'C' && c2 == 'P')
      return ValueWitnessKind::InitializeBufferWithCopyOfBuffer;
    if (c1 == 'C' && c2 == 'p')
      return ValueWitnessKind::InitializeBufferWithCopy;
    if (c1 == 'c' && c2 == 'p')
      return ValueWitnessKind::InitializeWithCopy;
    if (c1 == 'C' && c2 == 'c')
      return ValueWitnessKind::InitializeArrayWithCopy;
    if (c1 == 'T' && c2 == 'K')
      return ValueWitnessKind::InitializeBufferWithTakeOfBuffer;
    if (c1 == 'T' && c2 == 'k')
      return ValueWitnessKind::InitializeBufferWithTake;
    if (c1 == 't' && c2 == 'k')
      return ValueWitnessKind::InitializeWithTake;
    if (c1 == 'T' && c2 == 't')
      return ValueWitnessKind::InitializeArrayWithTakeFrontToBack;
    if (c1 == 't' && c2 == 'T')
      return ValueWitnessKind::InitializeArrayWithTakeBackToFront;
    if (c1 == 'p' && c2 == 'r')
      return ValueWitnessKind::ProjectBuffer;
    if (c1 == 'X' && c2 == 'x')
      return ValueWitnessKind::DestroyArray;
    if (c1 == 'x' && c2 == 's')
      return ValueWitnessKind::StoreExtraInhabitant;
    if (c1 == 'x' && c2 == 'g')
      return ValueWitnessKind::GetExtraInhabitantIndex;
    if (c1 == 'u' && c2 == 'g')
      return ValueWitnessKind::GetEnumTag;
    if (c1 == 'u' && c2 == 'p')
      return ValueWitnessKind::InplaceProjectEnumData;
    return ValueWitnessKind::Unknown;
  }

  NodePointer demangleGlobal() {
    if (!Mangled)
      return nullptr;

    // Type metadata.
    if (Mangled.nextIf('M')) {
      if (Mangled.nextIf('P')) {
        auto pattern =
            NodeFactory::create(Node::Kind::GenericTypeMetadataPattern);
        DEMANGLE_CHILD_AS_NODE_OR_RETURN(pattern, Directness);
        DEMANGLE_CHILD_OR_RETURN(pattern, Type);
        return pattern;
      }
      if (Mangled.nextIf('a')) {
        auto accessor =
          NodeFactory::create(Node::Kind::TypeMetadataAccessFunction);
        DEMANGLE_CHILD_OR_RETURN(accessor, Type);
        return accessor;
      }
      if (Mangled.nextIf('L')) {
        auto cache = NodeFactory::create(Node::Kind::TypeMetadataLazyCache);
        DEMANGLE_CHILD_OR_RETURN(cache, Type);
        return cache;
      }
      if (Mangled.nextIf('m')) {
        auto metaclass = NodeFactory::create(Node::Kind::Metaclass);
        DEMANGLE_CHILD_OR_RETURN(metaclass, Type);
        return metaclass;
      }
      if (Mangled.nextIf('n')) {
        auto nominalType =
            NodeFactory::create(Node::Kind::NominalTypeDescriptor);
        DEMANGLE_CHILD_OR_RETURN(nominalType, Type);
        return nominalType;
      }
      auto metadata = NodeFactory::create(Node::Kind::TypeMetadata);
      DEMANGLE_CHILD_AS_NODE_OR_RETURN(metadata, Directness);
      DEMANGLE_CHILD_OR_RETURN(metadata, Type);
      return metadata;
    }

    // Partial application thunks.
    if (Mangled.nextIf('P')) {
      if (!Mangled.nextIf('A')) return nullptr;
      Node::Kind kind = Node::Kind::PartialApplyForwarder;
      if (Mangled.nextIf('o'))
        kind = Node::Kind::PartialApplyObjCForwarder;
      auto forwarder = NodeFactory::create(kind);
      if (Mangled.nextIf("__T"))
        DEMANGLE_CHILD_OR_RETURN(forwarder, Global);
      return forwarder;
    }

    // Top-level types, for various consumers.
    if (Mangled.nextIf('t')) {
      return demangleType();
    }

    // Value witnesses.
    if (Mangled.nextIf('w')) {
      ValueWitnessKind w = demangleValueWitnessKind();
      if (w == ValueWitnessKind::Unknown)
        return nullptr;
      auto witness =
          NodeFactory::create(Node::Kind::ValueWitness, toString(w));
      DEMANGLE_CHILD_OR_RETURN(witness, Type);
      return witness;
    }

    // Offsets, value witness tables, and protocol witnesses.
    if (Mangled.nextIf('W')) {
      if (Mangled.nextIf('V')) {
        auto witnessTable = NodeFactory::create(Node::Kind::ValueWitnessTable);
        DEMANGLE_CHILD_OR_RETURN(witnessTable, Type);
        return witnessTable;
      }
      if (Mangled.nextIf('o')) {
        auto witnessTableOffset =
            NodeFactory::create(Node::Kind::WitnessTableOffset);
        DEMANGLE_CHILD_OR_RETURN(witnessTableOffset, Entity);
        return witnessTableOffset;
      }
      if (Mangled.nextIf('v')) {
        auto fieldOffset = NodeFactory::create(Node::Kind::FieldOffset);
        DEMANGLE_CHILD_AS_NODE_OR_RETURN(fieldOffset, Directness);
        DEMANGLE_CHILD_OR_RETURN(fieldOffset, Entity);
        return fieldOffset;
      }
      if (Mangled.nextIf('P')) {
        auto witnessTable =
            NodeFactory::create(Node::Kind::ProtocolWitnessTable);
        DEMANGLE_CHILD_OR_RETURN(witnessTable, ProtocolConformance);
        return witnessTable;
      }
      if (Mangled.nextIf('Z')) {
        auto accessor =
          NodeFactory::create(Node::Kind::LazyProtocolWitnessTableAccessor);
        DEMANGLE_CHILD_OR_RETURN(accessor, ProtocolConformance);
        return accessor;
      }
      if (Mangled.nextIf('z')) {
        auto tableTemplate =
          NodeFactory::create(Node::Kind::LazyProtocolWitnessTableTemplate);
        DEMANGLE_CHILD_OR_RETURN(tableTemplate, ProtocolConformance);
        return tableTemplate;
      }
      if (Mangled.nextIf('D')) {
        auto tableGenerator = NodeFactory::create(
            Node::Kind::DependentProtocolWitnessTableGenerator);
        DEMANGLE_CHILD_OR_RETURN(tableGenerator, ProtocolConformance);
        return tableGenerator;
      }
      if (Mangled.nextIf('d')) {
        auto tableTemplate = NodeFactory::create(
            Node::Kind::DependentProtocolWitnessTableTemplate);
        DEMANGLE_CHILD_OR_RETURN(tableTemplate, ProtocolConformance);
        return tableTemplate;
      }
      return nullptr;
    }

    // Other thunks.
    if (Mangled.nextIf('T')) {
      if (Mangled.nextIf('R')) {
        auto thunk = NodeFactory::create(Node::Kind::ReabstractionThunkHelper);
        if (!demangleReabstractSignature(thunk))
          return nullptr;
        return thunk;
      }
      if (Mangled.nextIf('r')) {
        auto thunk = NodeFactory::create(Node::Kind::ReabstractionThunk);
        if (!demangleReabstractSignature(thunk))
          return nullptr;
        return thunk;
      }
      if (Mangled.nextIf('W')) {
        NodePointer thunk = NodeFactory::create(Node::Kind::ProtocolWitness);
        DEMANGLE_CHILD_OR_RETURN(thunk, ProtocolConformance);
        DEMANGLE_CHILD_OR_RETURN(thunk, Entity);
        return thunk;
      }
      return nullptr;
    }

    // Everything else is just an entity.
    return demangleEntity();
  }

  NodePointer demangleGenericSpecialization() {
    auto specialization = NodeFactory::create(Node::Kind::SpecializedAttribute);
    auto kind = NodeFactory::create(Node::Kind::SpecializationKind, "generic");
    specialization->addChild(kind);

    while (!Mangled.nextIf('_')) {
      // Otherwise, we have another parameter. Demangle the type.
      NodePointer param = NodeFactory::create(Node::Kind::GenericSpecializationParam);
      NodePointer type = demangleType();
      if (!type)
        return nullptr;
      param->addChild(type);

      // Then parse any conformances until we find an underscore. Pop off the
      // underscore since it serves as the end of our mangling list.
      while (NodePointer conformance = demangleProtocolConformance()) {
        param->addChild(conformance);
      }

      // Add the parameter to our specialization list.
      specialization->addChild(param);
    }

    return specialization;
  }

  bool skipGlobal() {
    // Find how far we need to go to skip the global.
    StringRef before = Mangled.str();
    Demangler d(before);
    bool result = d.demangle();
    if (!result)
      return false;
    NodePointer n = d.getDemangled();
    unsigned suffixSize = n->getChild(n->getNumChildren()-1)->getText().size();
    Mangled.advanceOffset(before.size() - suffixSize);
    return true;
  }

#define FUNCSIGSPEC_CREATE_INFO_KIND(kind) \
  NodeFactory::create(Node::Kind::FunctionSignatureSpecializationParamInfo, \
                      uint8_t(FunctionSigSpecializationParamInfoKind::kind))

  NodePointer demangleFuncSigSpecializationConstantProp() {
    // Then figure out what was actually constant propagated. First check if
    // we have a function.
    if (Mangled.nextIf("fr")) {
      // Demangle the identifier
      if (!demangleIdentifier() || !Mangled.nextIf('_')) {
        return nullptr;
      }
      return FUNCSIGSPEC_CREATE_INFO_KIND(ConstantPropFunction);
    } else if (Mangled.nextIf('g')) {
      if (!demangleIdentifier() || !Mangled.nextIf('_')) {
        return nullptr;
      }
      return FUNCSIGSPEC_CREATE_INFO_KIND(ConstantPropGlobal);
    } else if (Mangled.nextIf('i')) {
      // Skip over the integer, we don't care about it.
      while (Mangled.nextIfNot('_'))
        continue;
      if (!Mangled.nextIf('_'))
        return nullptr;
      return FUNCSIGSPEC_CREATE_INFO_KIND(ConstantPropInteger);
    } else if (Mangled.nextIf("fl")) {
      // Skip over the float, we don't care about it.
      while (Mangled.nextIfNot('_'))
        continue;
      if (!Mangled.nextIf('_'))
        return nullptr;
      return FUNCSIGSPEC_CREATE_INFO_KIND(ConstantPropFloat);
    } else if (Mangled.nextIf("s")) {
      // Skip: 'e' encoding 'v' hash. encoding is a 0 or 1 and hash is a 32
      // bit hex MD5 sum of the contents.
      Mangled.advanceOffset(3);      
      if (!demangleIdentifier())
        return nullptr;
      if (!Mangled.nextIf('_'))
        return nullptr;
      return FUNCSIGSPEC_CREATE_INFO_KIND(ConstantPropString);
    }

    unreachable("Unknown constant prop specialization");
  }

  NodePointer demangleFuncSigSpecializationClosureProp() {
    // We don't actually demangle the function or types for now. But we do want
    // to signal that we specialized a closure.

    // If we don't, we must have a global. Skip it.
    if (!demangleIdentifier()) {
      return nullptr;
    }

    // Then demangle types until we fail.
    while (Mangled.peek() != '_' && demangleType()) {}

    // Eat last '_'
    if (!Mangled.nextIf('_'))
      return nullptr;

    return FUNCSIGSPEC_CREATE_INFO_KIND(ClosureProp);
  }

  NodePointer demangleFunctionSignatureSpecialization() {
    auto specialization = NodeFactory::create(Node::Kind::SpecializedAttribute);
    auto kind = NodeFactory::create(Node::Kind::SpecializationKind, "function signature");
    specialization->addChild(kind);

    unsigned paramCount = 0;

    // Until we hit the last '_' in our specialization info...
    while (!Mangled.nextIf('_')) {
      // Try to eat an n. If we succeed don't create any nodes and
      // continue. This is because we do not represent unmodified arguments in
      // the demangling.
      if (Mangled.nextIf("n_")) {
        paramCount++;
        continue;
      }

      // Create the parameter.
      NodePointer param =
        NodeFactory::create(Node::Kind::FunctionSignatureSpecializationParam,
                            paramCount);

      // Check if this parameter was constant propagated.
      if (Mangled.nextIf("cp")) {
        auto result = demangleFuncSigSpecializationConstantProp();
        if (!result)
          return nullptr;
        param->addChild(result);
        //if (!Mangled.nextIf('_'))
        //  return nullptr;
      } else if (Mangled.nextIf("cl")) {
        auto result = demangleFuncSigSpecializationClosureProp();
        if (!result)
          return nullptr;
        param->addChild(result);
        //if (!Mangled.nextIf('_'))
        //return nullptr;
      }

      if (!param || param->getNumChildren() == 0) {
        return nullptr;
      }
      specialization->addChild(param);
      paramCount++;
    }

    return specialization;
  }
#undef FUNCSIGSPEC_CREATE_INFO_KIND

  NodePointer demangleSpecializedAttribute() {
    if (Mangled.nextIf("g"))
      return demangleGenericSpecialization();
    if (Mangled.nextIf("f"))
      return demangleFunctionSignatureSpecialization();

    // For now just return a specialized attribute if we don't know what we are
    // demangling. We will assert in a later commit.
    return NodeFactory::create(Node::Kind::SpecializedAttribute);
  }
  
  NodePointer demangleDeclName() {
    // decl-name ::= local-decl-name
    // local-decl-name ::= 'L' index identifier
    if (Mangled.nextIf('L')) {
      NodePointer discriminator = demangleIndexAsNode();
      if (!discriminator) return nullptr;

      NodePointer name = demangleIdentifier();
      if (!name) return nullptr;

      NodePointer localName = NodeFactory::create(Node::Kind::LocalDeclName);
      localName->addChild(std::move(discriminator));
      localName->addChild(std::move(name));
      return localName;

    } else if (Mangled.nextIf('P')) {
      NodePointer discriminator = demangleIdentifier();
      if (!discriminator) return nullptr;

      NodePointer name = demangleIdentifier();
      if (!name) return nullptr;

      auto privateName = NodeFactory::create(Node::Kind::PrivateDeclName);
      privateName->addChildren(std::move(discriminator), std::move(name));
      return privateName;
    }

    // decl-name ::= identifier
    return demangleIdentifier();
  }

  NodePointer demangleIdentifier(Node::Kind kind = Node::Kind::Unknown) {
    if (!Mangled)
      return nullptr;
    
    bool isPunycoded = Mangled.nextIf('X');
    std::string decodeBuffer;

    auto decode = [&](StringRef s) -> StringRef {
      if (!isPunycoded)
        return s;
      if (!Punycode::decodePunycodeUTF8(s, decodeBuffer))
        return {};
      return decodeBuffer;
    };
    
    bool isOperator = false;
    if (Mangled.nextIf('o')) {
      isOperator = true;
      // Operator identifiers aren't valid in the contexts that are
      // building more specific identifiers.
      if (kind != Node::Kind::Unknown) return nullptr;

      char op_mode = Mangled.next();
      switch (op_mode) {
      case 'p':
        kind = Node::Kind::PrefixOperator;
        break;
      case 'P':
        kind = Node::Kind::PostfixOperator;
        break;
      case 'i':
        kind = Node::Kind::InfixOperator;
        break;
      default:
        return nullptr;
      }
    }

    if (kind == Node::Kind::Unknown) kind = Node::Kind::Identifier;

    Node::IndexType length;
    if (!demangleNatural(length))
      return nullptr;
    if (!Mangled.hasAtLeast(length))
      return nullptr;
    
    StringRef identifier = Mangled.slice(length);
    Mangled.advanceOffset(length);
    
    // Decode Unicode identifiers.
    identifier = decode(identifier);
    if (identifier.empty())
      return nullptr;

    // Decode operator names.
    std::string opDecodeBuffer;
    if (isOperator) {
                                        // abcdefghijklmnopqrstuvwxyz
      static const char op_char_table[] = "& @/= >    <*!|+?%-~   ^ .";

      opDecodeBuffer.reserve(identifier.size());
      for (signed char c : identifier) {
        if (c < 0) {
          // Pass through Unicode characters.
          opDecodeBuffer.push_back(c);
          continue;
        }
        if (c < 'a' || c > 'z')
          return nullptr;
        char o = op_char_table[c - 'a'];
        if (o == ' ')
          return nullptr;
        opDecodeBuffer.push_back(o);
      }
      identifier = opDecodeBuffer;
    }
    
    return NodeFactory::create(kind, identifier);
  }

  bool demangleIndex(Node::IndexType &natural) {
    if (Mangled.nextIf('_')) {
      natural = 0;
      return true;
    }
    if (demangleNatural(natural)) {
      if (!Mangled.nextIf('_'))
        return false;
      natural++;
      return true;
    }
    return false;
  }

  /// Demangle an <index> and package it as a node of some kind.
  NodePointer demangleIndexAsNode(Node::Kind kind = Node::Kind::Number) {
    Node::IndexType index;
    if (!demangleIndex(index))
      return nullptr;
    return NodeFactory::create(kind, index);
  }

  NodePointer createSwiftType(Node::Kind typeKind, StringRef name) {
    NodePointer type = NodeFactory::create(typeKind);
    type->addChild(NodeFactory::create(Node::Kind::Module, STDLIB_NAME));
    type->addChild(NodeFactory::create(Node::Kind::Identifier, name));
    return type;
  }

  /// Demangle a <substitution>, given that we've already consumed the 'S'.
  NodePointer demangleSubstitutionIndex() {
    if (!Mangled)
      return NodeFactory::create(Node::Kind::Failure);
    if (Mangled.nextIf('o'))
      return NodeFactory::create(Node::Kind::Module, "ObjectiveC");
    if (Mangled.nextIf('C'))
      return NodeFactory::create(Node::Kind::Module, "C");
    if (Mangled.nextIf('s'))
      return NodeFactory::create(Node::Kind::Module, STDLIB_NAME);
    if (Mangled.nextIf('a'))
      return createSwiftType(Node::Kind::Structure, "Array");
    if (Mangled.nextIf('b'))
      return createSwiftType(Node::Kind::Structure, "Bool");
    if (Mangled.nextIf('c'))
      return createSwiftType(Node::Kind::Structure, "UnicodeScalar");
    if (Mangled.nextIf('d'))
      return createSwiftType(Node::Kind::Structure, "Double");
    if (Mangled.nextIf('f'))
      return createSwiftType(Node::Kind::Structure, "Float");
    if (Mangled.nextIf('i'))
      return createSwiftType(Node::Kind::Structure, "Int");
    if (Mangled.nextIf('q'))
      return createSwiftType(Node::Kind::Enum, "Optional");
    if (Mangled.nextIf('Q'))
      return createSwiftType(Node::Kind::Enum, "ImplicitlyUnwrappedOptional");
    if (Mangled.nextIf('S'))
      return createSwiftType(Node::Kind::Structure, "String");
    if (Mangled.nextIf('u'))
      return createSwiftType(Node::Kind::Structure, "UInt");
    Node::IndexType index_sub;
    if (!demangleIndex(index_sub))
      return NodeFactory::create(Node::Kind::Failure);
    if (index_sub >= Substitutions.size())
      return NodeFactory::create(Node::Kind::Failure);
    return Substitutions[index_sub];
  }

  NodePointer demangleModule() {
    if (Mangled.nextIf('S')) {
      NodePointer module = demangleSubstitutionIndex();
      if (!module)
        return nullptr;
      if (module->getKind() != Node::Kind::Module)
        return nullptr;
      return module;
    }

    NodePointer module = demangleIdentifier(Node::Kind::Module);
    if (!module) return nullptr;
    Substitutions.push_back(module);
    return module;
  }

  NodePointer demangleDeclarationName(Node::Kind kind) {
    NodePointer context = demangleContext();
    if (!context) return nullptr;

    auto name = demangleDeclName();
    if (!name) return nullptr;

    auto decl = NodeFactory::create(kind);
    decl->addChild(context);
    decl->addChild(name);
    Substitutions.push_back(decl);
    return decl;
  }

  NodePointer demangleProtocolName() {
    NodePointer proto = demangleProtocolNameImpl();
    if (!proto) return nullptr;

    NodePointer type = NodeFactory::create(Node::Kind::Type);
    type->addChild(proto);
    return type;
  }
  
  NodePointer demangleProtocolNameImpl() {
    // There's an ambiguity in <protocol> between a substitution of
    // the protocol and a substitution of the protocol's context, so
    // we have to duplicate some of the logic from
    // demangleDeclarationName.
    if (Mangled.nextIf('S')) {
      NodePointer sub = demangleSubstitutionIndex();
      if (!sub) return nullptr;
      if (sub->getKind() == Node::Kind::Protocol)
        return sub;

      if (sub->getKind() != Node::Kind::Module)
        return nullptr;

      NodePointer name = demangleDeclName();
      if (!name) return nullptr;

      auto proto = NodeFactory::create(Node::Kind::Protocol);
      proto->addChild(std::move(sub));
      proto->addChild(std::move(name));
      Substitutions.push_back(proto);
      return proto;
    }

    return demangleDeclarationName(Node::Kind::Protocol);
  }

  NodePointer demangleNominalType() {
    if (Mangled.nextIf('S'))
      return demangleSubstitutionIndex();
    if (Mangled.nextIf('V'))
      return demangleDeclarationName(Node::Kind::Structure);
    if (Mangled.nextIf('O'))
      return demangleDeclarationName(Node::Kind::Enum);
    if (Mangled.nextIf('C'))
      return demangleDeclarationName(Node::Kind::Class);
    if (Mangled.nextIf('P'))
      return demangleDeclarationName(Node::Kind::Protocol);
    return nullptr;
  }

  NodePointer demangleContext() {
    // context ::= module
    // context ::= entity
    // context ::= E module (extension defined in a different module)
    if (!Mangled) return nullptr;
    if (Mangled.nextIf('E')) {
      NodePointer ext = NodeFactory::create(Node::Kind::Extension);
      NodePointer def_module = demangleModule();
      NodePointer type = demangleContext();
      ext->addChild(def_module);
      ext->addChild(type);
      return ext;
    }
    if (Mangled.nextIf('S'))
      return demangleSubstitutionIndex();
    if (isStartOfEntity(Mangled.peek()))
      return demangleEntity();
    return demangleModule();
  }
  
  NodePointer demangleProtocolList() {
    NodePointer proto_list = NodeFactory::create(Node::Kind::ProtocolList);
    NodePointer type_list = NodeFactory::create(Node::Kind::TypeList);
    proto_list->addChild(type_list);
    if (Mangled.nextIf('_')) {
      return proto_list;
    }
    NodePointer proto = demangleProtocolName();
    if (!proto)
      return nullptr;
    type_list->addChild(proto);
    while (Mangled.nextIf('_') == false) {
      proto = demangleProtocolName();
      if (!proto)
        return nullptr;
      type_list->addChild(proto);
    }
    return proto_list;
  }

  NodePointer demangleProtocolConformance() {
    NodePointer type = demangleType();
    if (!type)
      return nullptr;
    NodePointer protocol = demangleProtocolName();
    if (!protocol)
      return nullptr;
    NodePointer context = demangleContext();
    if (!context)
      return nullptr;
    NodePointer proto_conformance =
        NodeFactory::create(Node::Kind::ProtocolConformance);
    proto_conformance->addChild(type);
    proto_conformance->addChild(protocol);
    proto_conformance->addChild(context);
    return proto_conformance;
  }

  // entity ::= entity-kind context entity-name
  // entity ::= nominal-type
  NodePointer demangleEntity() {
    // entity-kind
    Node::Kind entityBasicKind;
    if (Mangled.nextIf('F')) {
      entityBasicKind = Node::Kind::Function;
    } else if (Mangled.nextIf('v')) {
      entityBasicKind = Node::Kind::Variable;
    } else if (Mangled.nextIf('I')) {
      entityBasicKind = Node::Kind::Initializer;
    } else if (Mangled.nextIf('s')) {
      entityBasicKind = Node::Kind::Subscript;
    } else {
      return demangleNominalType();
    }

    NodePointer context = demangleContext();
    if (!context) return nullptr;

    // entity-name
    Node::Kind entityKind;
    bool hasType = true;
    NodePointer name;
    if (Mangled.nextIf('D')) {
      if (context->getKind() == Node::Kind::Class)
        entityKind = Node::Kind::Deallocator;
      else
        entityKind = Node::Kind::Destructor;
      hasType = false;
    } else if (Mangled.nextIf('d')) {
      entityKind = Node::Kind::Destructor;
      hasType = false;
    } else if (Mangled.nextIf('e')) {
      entityKind = Node::Kind::IVarInitializer;
      hasType = false;
    } else if (Mangled.nextIf('E')) {
      entityKind = Node::Kind::IVarDestroyer;
      hasType = false;
    } else if (Mangled.nextIf('C')) {
      if (context->getKind() == Node::Kind::Class)
        entityKind = Node::Kind::Allocator;
      else
        entityKind = Node::Kind::Constructor;
    } else if (Mangled.nextIf('c')) {
      entityKind = Node::Kind::Constructor;
    } else if (Mangled.nextIf('a')) {
      entityKind = Node::Kind::MutableAddressor;
      name = demangleDeclName();
      if (!name) return nullptr;
    } else if (Mangled.nextIf('l')) {
      entityKind = Node::Kind::Addressor;
      name = demangleDeclName();
      if (!name) return nullptr;
    } else if (Mangled.nextIf('g')) {
      entityKind = Node::Kind::Getter;
      name = demangleDeclName();
      if (!name) return nullptr;
    } else if (Mangled.nextIf('G')) {
      entityKind = Node::Kind::GlobalGetter;
      name = demangleDeclName();
      if (!name) return nullptr;
    } else if (Mangled.nextIf('s')) {
      entityKind = Node::Kind::Setter;
      name = demangleDeclName();
      if (!name) return nullptr;
    } else if (Mangled.nextIf('m')) {
      entityKind = Node::Kind::MaterializeForSet;
      name = demangleDeclName();
      if (!name) return nullptr;
    } else if (Mangled.nextIf('w')) {
      entityKind = Node::Kind::WillSet;
      name = demangleDeclName();
      if (!name) return nullptr;
    } else if (Mangled.nextIf('W')) {
      entityKind = Node::Kind::DidSet;
      name = demangleDeclName();
      if (!name) return nullptr;
    } else if (Mangled.nextIf('U')) {
      entityKind = Node::Kind::ExplicitClosure;
      name = demangleIndexAsNode();
      if (!name) return nullptr;
    } else if (Mangled.nextIf('u')) {
      entityKind = Node::Kind::ImplicitClosure;
      name = demangleIndexAsNode();
      if (!name) return nullptr;
    } else if (entityBasicKind == Node::Kind::Initializer) {
      // entity-name ::= 'A' index
      if (Mangled.nextIf('A')) {
        entityKind = Node::Kind::DefaultArgumentInitializer;
        name = demangleIndexAsNode();
        if (!name) return nullptr;
      // entity-name ::= 'i'
      } else if (Mangled.nextIf('i')) {
        entityKind = Node::Kind::Initializer;
      } else {
        return nullptr;
      }
      hasType = false;
    } else {
      entityKind = entityBasicKind;
      name = demangleDeclName();
      if (!name) return nullptr;
    }

    NodePointer entity = NodeFactory::create(entityKind);
    entity->addChild(context);

    if (name) entity->addChild(name);

    if (hasType) {
      auto type = demangleType();
      if (!type) return nullptr;
      entity->addChild(type);
    }

    return entity;
  }

  /// A RAII object designed for parsing generic signatures.
  class GenericContext {
    Demangler &D;
  public:
    GenericContext(Demangler &D) : D(D) {
      D.ArchetypeCounts.push_back(D.ArchetypeCount);
    }
    ~GenericContext() {
      D.ArchetypeCount = D.ArchetypeCounts.back();
    }
  };

  /// Demangle a generic clause.
  ///
  /// \param C - not really required; just a token to prove that the caller
  ///   has thought to enter a generic context
  NodePointer demangleGenerics(GenericContext &C) {
    DemanglerPrinter result_printer;
    NodePointer archetypes = NodeFactory::create(Node::Kind::Generics);
    // FIXME: Swallow the mangled associated type constraints.
    bool assocTypes = false;
    while (true) {
      if (!assocTypes && Mangled.nextIf('U')) {
        assocTypes = true;
        continue;
      }
      if (Mangled.nextIf('_')) {
        if (!Mangled)
          return nullptr;
        char c = Mangled.peek();
        if (c != '_' && c != 'S'
            && (assocTypes || c != 'U')
            && !isStartOfIdentifier(c))
          break;
        if (!assocTypes)
          archetypes->addChild(NodeFactory::create(
              Node::Kind::ArchetypeRef, archetypeName(ArchetypeCount)));
      } else {
        NodePointer proto_list = demangleProtocolList();
        if (!proto_list)
          return nullptr;
        if (assocTypes)
          continue;
        NodePointer arch_and_proto =
            NodeFactory::create(Node::Kind::ArchetypeAndProtocol);
        arch_and_proto->addChild(NodeFactory::create(
            Node::Kind::ArchetypeRef, archetypeName(ArchetypeCount)));
        arch_and_proto->addChild(proto_list);
        archetypes->addChild(arch_and_proto);
      }
      ++ArchetypeCount;
    }
    return archetypes;
  }

  NodePointer demangleArchetypeRef(Node::IndexType depth, Node::IndexType i) {
    if (depth == 0 && ArchetypeCount == 0)
      return NodeFactory::create(Node::Kind::ArchetypeRef, archetypeName(i));
    size_t length = ArchetypeCounts.size();
    if (depth >= length)
      return nullptr;
    size_t index = ArchetypeCounts[length - 1 - depth] + i;
    size_t max =
        (depth == 0) ? ArchetypeCount : ArchetypeCounts[length - depth];
    if (index >= max)
      return nullptr;
    return NodeFactory::create(Node::Kind::ArchetypeRef,
                                 archetypeName(index));
  }
  
  NodePointer demangleDependentType() {
    // A dependent member type begins with a non-index, non-'d' character.
    auto c = Mangled.peek();
    if (c != 'd' && c != '_' && !isdigit(c)) {
      NodePointer baseType = demangleType();
      if (!baseType) return nullptr;
      NodePointer depTy = demangleIdentifier(Node::Kind::DependentMemberType);
      if (!depTy) return nullptr;
      
      depTy->addChild(baseType);
      return depTy;
    }
    
    // Otherwise, we have a generic parameter.
    Node::IndexType depth, index;
    if (Mangled.nextIf('d')) {
      if (!demangleIndex(depth))
        return nullptr;
      depth += 1;
      if (!demangleIndex(index))
        return nullptr;
    } else {
      depth = 0;
      if (!demangleIndex(index))
        return nullptr;
    }
    
    DemanglerPrinter Name;
    Name << "T_" << depth << '_' << index;

    auto paramTy = NodeFactory::create(Node::Kind::DependentGenericParamType,
                                       std::move(Name.str()));
    return paramTy;
  }
  
  NodePointer demangleGenericSignature() {
    auto sig = NodeFactory::create(Node::Kind::DependentGenericSignature);
    // First read in the parameter counts at each depth.
    while (!Mangled.nextIf('R')) {
      Node::IndexType count;
      if (!demangleIndex(count))
        return nullptr;
      auto countNode =
          NodeFactory::create(Node::Kind::DependentGenericParamCount, count);
      sig->addChild(countNode);
    }
    
    // Next read in the generic requirements.
    while (!Mangled.nextIf('_')) {
      NodePointer reqt = demangleGenericRequirement();
      if (!reqt) return nullptr;
      sig->addChild(reqt);
    }
    
    return sig;
  }
  
  NodePointer demangleGenericRequirement() {
    if (Mangled.nextIf('P')) {
      NodePointer type = demangleType();
      if (!type) return nullptr;
      NodePointer requirement = demangleType();
      if (!requirement) return nullptr;
      auto reqt = NodeFactory::create(
          Node::Kind::DependentGenericConformanceRequirement);
      reqt->addChild(type);
      reqt->addChild(requirement);
      return reqt;
    }
    if (Mangled.nextIf('E')) {
      NodePointer first = demangleType();
      if (!first) return nullptr;
      NodePointer second = demangleType();
      if (!second) return nullptr;
      auto reqt = NodeFactory::create(
          Node::Kind::DependentGenericSameTypeRequirement);
      reqt->addChild(first);
      reqt->addChild(second);
      return reqt;
    }
    return nullptr;
  }
  
  NodePointer demangleArchetypeType() {
    auto makeSelfType = [&](NodePointer proto) -> NodePointer {
      auto selfType = NodeFactory::create(Node::Kind::SelfTypeRef);
      selfType->addChild(proto);
      Substitutions.push_back(selfType);
      return selfType;
    };
    
    auto makeAssociatedType = [&](NodePointer root) -> NodePointer {
      NodePointer name = demangleIdentifier();
      if (!name) return nullptr;
      auto assocType = NodeFactory::create(Node::Kind::AssociatedTypeRef);
      assocType->addChild(root);
      assocType->addChild(name);
      Substitutions.push_back(assocType);
      return assocType;
    };
    
    if (Mangled.nextIf('P')) {
      NodePointer proto = demangleProtocolName();
      if (!proto) return nullptr;
      return makeSelfType(proto);
    }
    
    if (Mangled.nextIf('Q')) {
      NodePointer root = demangleArchetypeType();
      if (!root) return nullptr;
      return makeAssociatedType(root);
    }
    if (Mangled.nextIf('S')) {
      NodePointer sub = demangleSubstitutionIndex();
      if (!sub) return nullptr;
      if (sub->getKind() == Node::Kind::Protocol)
        return makeSelfType(sub);
      else
        return makeAssociatedType(sub);
    }
    if (Mangled.nextIf('d')) {
      Node::IndexType depth, index;
      if (!demangleIndex(depth))
        return nullptr;
      if (!demangleIndex(index))
        return nullptr;
      return demangleArchetypeRef(depth + 1, index);
    }
    if (Mangled.nextIf('q')) {
      NodePointer index = demangleIndexAsNode();
      if (!index)
        return nullptr;
      NodePointer decl_ctx = NodeFactory::create(Node::Kind::DeclContext);
      NodePointer ctx = demangleContext();
      if (!ctx)
        return nullptr;
      decl_ctx->addChild(ctx);
      auto qual_atype = NodeFactory::create(Node::Kind::QualifiedArchetype);
      qual_atype->addChild(index);
      qual_atype->addChild(decl_ctx);
      return qual_atype;
    }
    Node::IndexType index;
    if (!demangleIndex(index))
      return nullptr;
    return demangleArchetypeRef(0, index);
  }

  NodePointer demangleTuple(IsVariadic isV) {
    NodePointer tuple = NodeFactory::create(
        isV == IsVariadic::yes ? Node::Kind::VariadicTuple
                               : Node::Kind::NonVariadicTuple);
    while (!Mangled.nextIf('_')) {
      if (!Mangled)
        return nullptr;
      NodePointer elt = NodeFactory::create(Node::Kind::TupleElement);

      if (isStartOfIdentifier(Mangled.peek())) {
        NodePointer label = demangleIdentifier(Node::Kind::TupleElementName);
        if (!label)
          return nullptr;
        elt->addChild(label);
      }

      NodePointer type = demangleType();
      if (!type)
        return nullptr;
      elt->addChild(type);

      tuple->addChild(elt);
    }
    return tuple;
  }
  
  NodePointer postProcessReturnTypeNode (NodePointer out_args) {
    NodePointer out_node = NodeFactory::create(Node::Kind::ReturnType);
    out_node->addChild(out_args);
    return out_node;
  }

  NodePointer demangleType() {
    NodePointer type = demangleTypeImpl();
    if (!type)
      return nullptr;
    NodePointer nodeType = NodeFactory::create(Node::Kind::Type);
    nodeType->addChild(type);
    return nodeType;
  }
  
  NodePointer demangleFunctionType(Node::Kind kind) {
    NodePointer in_args = demangleType();
    if (!in_args)
      return nullptr;
    NodePointer out_args = demangleType();
    if (!out_args)
      return nullptr;
    NodePointer block = NodeFactory::create(kind);
    NodePointer in_node = NodeFactory::create(Node::Kind::ArgumentTuple);
    block->addChild(in_node);
    in_node->addChild(in_args);
    block->addChild(postProcessReturnTypeNode(out_args));
    return block;
  }
  
  NodePointer demangleTypeImpl() {
    if (!Mangled)
      return nullptr;
    char c = Mangled.next();
    if (c == 'B') {
      if (!Mangled)
        return nullptr;
      c = Mangled.next();
      if (c == 'f') {
        Node::IndexType size;
        if (demangleBuiltinSize(size)) {
          return NodeFactory::create(
              Node::Kind::BuiltinTypeName,
              (DemanglerPrinter() << "Builtin.Float" << size).str());
        }
      }
      if (c == 'i') {
        Node::IndexType size;
        if (demangleBuiltinSize(size)) {
          return NodeFactory::create(
              Node::Kind::BuiltinTypeName,
              (DemanglerPrinter() << "Builtin.Int" << size).str());
        }
      }
      if (c == 'v') {
        Node::IndexType elts;
        if (demangleNatural(elts)) {
          if (!Mangled.nextIf('B'))
            return nullptr;
          if (Mangled.nextIf('i')) {
            Node::IndexType size;
            if (!demangleBuiltinSize(size))
              return nullptr;
            return NodeFactory::create(
                Node::Kind::BuiltinTypeName,
                (DemanglerPrinter() << "Builtin.Vec" << elts << "xInt" << size)
                    .str());
          }
          if (Mangled.nextIf('f')) {
            Node::IndexType size;
            if (!demangleBuiltinSize(size))
              return nullptr;
            return NodeFactory::create(
                Node::Kind::BuiltinTypeName,
                (DemanglerPrinter() << "Builtin.Vec" << elts << "xFloat"
                                    << size).str());
          }
          if (Mangled.nextIf('p'))
            return NodeFactory::create(
                Node::Kind::BuiltinTypeName,
                (DemanglerPrinter() << "Builtin.Vec" << elts << "xRawPointer")
                    .str());
        }
      }
      if (c == 'O')
        return NodeFactory::create(Node::Kind::BuiltinTypeName,
                                     "Builtin.UnknownObject");
      if (c == 'o')
        return NodeFactory::create(Node::Kind::BuiltinTypeName,
                                     "Builtin.NativeObject");
      if (c == 'b')
        return NodeFactory::create(Node::Kind::BuiltinTypeName,
                                     "Builtin.BridgeObject");
      if (c == 'p')
        return NodeFactory::create(Node::Kind::BuiltinTypeName,
                                     "Builtin.RawPointer");
      if (c == 'w')
        return NodeFactory::create(Node::Kind::BuiltinTypeName,
                                     "Builtin.Word");
      return nullptr;
    }
    if (c == 'a')
      return demangleDeclarationName(Node::Kind::TypeAlias);

    if (c == 'b') {
      NodePointer in_args = demangleType();
      if (!in_args)
        return nullptr;
      NodePointer out_args = demangleType();
      if (!out_args)
        return nullptr;
      NodePointer block = NodeFactory::create(Node::Kind::ObjCBlock);
      NodePointer in_node = NodeFactory::create(Node::Kind::ArgumentTuple);
      block->addChild(in_node);
      in_node->addChild(in_args);
      block->addChild(postProcessReturnTypeNode(out_args));
      return block;
    }
    if (c == 'D') {
      NodePointer type = demangleType();
      if (!type)
        return nullptr;

      NodePointer dynamicSelf = NodeFactory::create(Node::Kind::DynamicSelf);
      dynamicSelf->addChild(type);
      return dynamicSelf;
    }
    if (c == 'E') {
      if (!Mangled.nextIf('R'))
        return nullptr;
      if (!Mangled.nextIf('R'))
        return nullptr;
      return NodeFactory::create(Node::Kind::ErrorType, std::string());
    }
    if (c == 'F') {
      return demangleFunctionType(Node::Kind::FunctionType);
    }
    if (c == 'f') {
      NodePointer in_args = demangleTypeImpl();
      if (!in_args)
        return nullptr;
      NodePointer out_args = demangleType();
      if (!out_args)
        return nullptr;
      NodePointer block =
          NodeFactory::create(Node::Kind::UncurriedFunctionType);
      block->addChild(in_args);
      block->addChild(postProcessReturnTypeNode(out_args));
      return block;
    }
    if (c == 'G') {
      NodePointer type_list = NodeFactory::create(Node::Kind::TypeList);
      NodePointer unboundType = demangleType();
      if (!unboundType)
        return nullptr;
      if (Mangled.isEmpty())
        return nullptr;
      while (Mangled.peek() != '_') {
        NodePointer type = demangleType();
        if (!type)
          return nullptr;
        type_list->addChild(type);
        if (Mangled.isEmpty())
          return nullptr;
      }
      Mangled.next();
      Node::Kind bound_type_kind = Node::Kind::Unknown;
      switch (unboundType->getChild(0)->getKind()) { // look through Type node
        case Node::Kind::Class:
          bound_type_kind = Node::Kind::BoundGenericClass;
          break;
        case Node::Kind::Structure:
          bound_type_kind = Node::Kind::BoundGenericStructure;
          break;
        case Node::Kind::Enum:
          bound_type_kind = Node::Kind::BoundGenericEnum;
          break;
        default:
          assert(false && "trying to make a generic type application for a non class|struct|enum");
      }
      NodePointer type_application =
          NodeFactory::create(bound_type_kind);
      type_application->addChild(unboundType);
      type_application->addChild(type_list);
      return type_application;
    }
    if (c == 'K') {
      return demangleFunctionType(Node::Kind::AutoClosureType);
    }
    if (c == 'M') {
      NodePointer type = demangleType();
      if (!type)
        return nullptr;
      NodePointer metatype = NodeFactory::create(Node::Kind::Metatype);
      metatype->addChild(type);
      return metatype;
    }
    if (c == 'P') {
      if (Mangled.nextIf('M')) {
        NodePointer type = demangleType();
        if (!type) return nullptr;
        auto metatype = NodeFactory::create(Node::Kind::ExistentialMetatype);
        metatype->addChild(type);
        return metatype;
      }

      return demangleProtocolList();
    }
    if (c == 'Q') {
      return demangleArchetypeType();
    }
    if (c == 'q') {
      return demangleDependentType();
    }
    if (c == 'R') {
      NodePointer inout = NodeFactory::create(Node::Kind::InOut);
      NodePointer type = demangleTypeImpl();
      if (!type)
        return nullptr;
      inout->addChild(type);
      return inout;
    }
    if (c == 'S') {
      return demangleSubstitutionIndex();
    }
    if (c == 'T') {
      return demangleTuple(IsVariadic::no);
    }
    if (c == 't') {
      return demangleTuple(IsVariadic::yes);
    }
    if (c == 'u') {
      NodePointer sig = demangleGenericSignature();
      if (!sig) return nullptr;
      NodePointer sub = demangleType();
      if (!sub) return nullptr;
      NodePointer dependentGenericType
        = NodeFactory::create(Node::Kind::DependentGenericType);
      dependentGenericType->addChild(sig);
      dependentGenericType->addChild(sub);
      return dependentGenericType;
    }
    if (c == 'U') {
      GenericContext genericContext(*this);
      NodePointer generics = demangleGenerics(genericContext);
      if (!generics)
        return nullptr;
      NodePointer base = demangleType();
      if (!base)
        return nullptr;
      NodePointer genericType = NodeFactory::create(Node::Kind::GenericType);
      genericType->addChild(generics);
      genericType->addChild(base);
      return genericType;
    }
    if (c == 'X') {
      if (Mangled.nextIf('o')) {
        NodePointer type = demangleType();
        if (!type)
          return nullptr;
        NodePointer unowned = NodeFactory::create(Node::Kind::Unowned);
        unowned->addChild(type);
        return unowned;
      }
      if (Mangled.nextIf('u')) {
        NodePointer type = demangleType();
        if (!type)
          return nullptr;
        NodePointer unowned = NodeFactory::create(Node::Kind::Unmanaged);
        unowned->addChild(type);
        return unowned;
      }
      if (Mangled.nextIf('w')) {
        NodePointer type = demangleType();
        if (!type)
          return nullptr;
        NodePointer weak = NodeFactory::create(Node::Kind::Weak);
        weak->addChild(type);
        return weak;
      }

      // type ::= 'XF' impl-function-type
      if (Mangled.nextIf('F')) {
        return demangleImplFunctionType();
      }

      return nullptr;
    }
    if (isStartOfNominalType(c)) {
      NodePointer nominal_type = demangleDeclarationName(nominalTypeMarkerToNodeKind(c));
      return nominal_type;
    }
    return nullptr;
  }

  bool demangleReabstractSignature(NodePointer signature) {
    if (Mangled.nextIf('G')) {
      NodePointer generics = demangleGenericSignature();
      if (!generics) return failure();
      signature->addChild(std::move(generics));
    }

    NodePointer srcType = demangleType();
    if (!srcType) return failure();
    signature->addChild(std::move(srcType));

    NodePointer destType = demangleType();
    if (!destType) return failure();
    signature->addChild(std::move(destType));

    return true;
  }

  // impl-function-type ::= impl-callee-convention impl-function-attribute*
  //                        generics? '_' impl-parameter* '_' impl-result* '_'
  // impl-function-attribute ::= 'Cb'            // compatible with C block invocation function
  // impl-function-attribute ::= 'Cc'            // compatible with C global function
  // impl-function-attribute ::= 'Cm'            // compatible with Swift method
  // impl-function-attribute ::= 'CO'            // compatible with ObjC method
  // impl-function-attribute ::= 'Cw'            // compatible with protocol witness
  // impl-function-attribute ::= 'N'             // noreturn
  // impl-function-attribute ::= 'G'             // generic
  NodePointer demangleImplFunctionType() {
    NodePointer type = NodeFactory::create(Node::Kind::ImplFunctionType);

    if (!demangleImplCalleeConvention(type))
      return nullptr;

    if (Mangled.nextIf('C')) {
      if (Mangled.nextIf('b'))
        addImplFunctionAttribute(type, "@objc_block");
      else if (Mangled.nextIf('c'))
        addImplFunctionAttribute(type, "@cc(cdecl)");
      else if (Mangled.nextIf('m'))
        addImplFunctionAttribute(type, "@cc(method)");
      else if (Mangled.nextIf('O'))
        addImplFunctionAttribute(type, "@cc(objc_method)");
      else if (Mangled.nextIf('w'))
        addImplFunctionAttribute(type, "@cc(witness_method)");
      else
        return nullptr;
    }

    if (Mangled.nextIf('N'))
      addImplFunctionAttribute(type, "@noreturn");

    // Enter a new generic context if this type is generic.
    // FIXME: replace with std::optional, when we have it.
    std::vector<GenericContext> genericContext;
    if (Mangled.nextIf('G')) {
      genericContext.emplace_back(*this);
      NodePointer generics = demangleGenerics(genericContext.front());
      if (!generics)
        return nullptr;
      type->addChild(generics);
    }

    // Expect the attribute terminator.
    if (!Mangled.nextIf('_'))
      return nullptr;

    // Demangle the parameters.
    if (!demangleImplParameters(type))
      return nullptr;

    // Demangle the result type.
    if (!demangleImplResults(type))
      return nullptr;

    return type;
  }

  enum class ImplConventionContext { Callee, Parameter, Result };

  /// impl-convention ::= 'a'                     // direct, autoreleased
  /// impl-convention ::= 'd'                     // direct, no ownership transfer
  /// impl-convention ::= 'D'                     // direct, no ownership transfer,
  ///                                             // dependent on self
  /// impl-convention ::= 'g'                     // direct, guaranteed
  /// impl-convention ::= 'i'                     // indirect, ownership transfer
  /// impl-convention ::= 'l'                     // indirect, inout
  /// impl-convention ::= 'o'                     // direct, ownership transfer
  ///
  /// Returns an empty string otherwise.
  StringRef demangleImplConvention(ImplConventionContext ctxt) {
#define CASE(CHAR, FOR_CALLEE, FOR_PARAMETER, FOR_RESULT)            \
    if (Mangled.nextIf(CHAR)) {                                      \
      switch (ctxt) {                                                \
      case ImplConventionContext::Callee: return (FOR_CALLEE);       \
      case ImplConventionContext::Parameter: return (FOR_PARAMETER); \
      case ImplConventionContext::Result: return (FOR_RESULT);       \
      }                                                              \
      unreachable("bad context");                               \
    }
    auto Nothing = StringRef();
    CASE('a',   Nothing,                Nothing,         "@autoreleased")
    CASE('d',   "@callee_unowned",      "@unowned",      "@unowned")
    CASE('d',   Nothing,                Nothing,         "@unowned_inner_pointer")
    CASE('g',   "@callee_guaranteed",   "@guaranteed",   Nothing)
    CASE('i',   Nothing,                "@in",           "@out")
    CASE('l',   Nothing,                "@inout",        Nothing)
    CASE('o',   "@callee_owned",        "@owned",        "@owned")
    return Nothing;
#undef CASE
  }

  // impl-callee-convention ::= 't'
  // impl-callee-convention ::= impl-convention
  bool demangleImplCalleeConvention(NodePointer type) {
    StringRef attr;
    if (Mangled.nextIf('t')) {
      attr = "@thin";
    } else {
      attr = demangleImplConvention(ImplConventionContext::Callee);
    }
    if (attr.empty()) {
      return failure();
    }
    type->addChild(NodeFactory::create(Node::Kind::ImplConvention, attr));
    return true;
  }

  void addImplFunctionAttribute(NodePointer parent, StringRef attr,
                         Node::Kind kind = Node::Kind::ImplFunctionAttribute) {
    parent->addChild(NodeFactory::create(kind, attr));
  }

  // impl-parameter ::= impl-convention type
  bool demangleImplParameters(NodePointer parent) {
    while (!Mangled.nextIf('_')) {
      auto input = demangleImplParameterOrResult(Node::Kind::ImplParameter);
      if (!input) return false;
      parent->addChild(input);
    }
    return true;
  }

  // impl-result ::= impl-convention type
  bool demangleImplResults(NodePointer parent) {
    while (!Mangled.nextIf('_')) {
      auto res = demangleImplParameterOrResult(Node::Kind::ImplResult);
      if (!res) return false;
      parent->addChild(res);
    }
    return true;
  }

  NodePointer demangleImplParameterOrResult(Node::Kind kind) {
    auto getContext = [](Node::Kind kind) -> ImplConventionContext {
      if (kind == Node::Kind::ImplParameter)
        return ImplConventionContext::Parameter;
      else if (kind == Node::Kind::ImplResult)
        return ImplConventionContext::Result;
      else
        unreachable("unexpected node kind");
    };

    auto convention = demangleImplConvention(getContext(kind));
    if (convention.empty()) return nullptr;
    auto type = demangleType();
    if (!type) return nullptr;

    NodePointer node = NodeFactory::create(kind);
    node->addChild(NodeFactory::create(Node::Kind::ImplConvention,
                                       convention));
    node->addChild(type);
    return node;
  }
};
} // end anonymous namespace

NodePointer
swift::Demangle::demangleSymbolAsNode(const char *MangledName,
                                      size_t MangledNameLength,
                                      const DemangleOptions &Options) {
  Demangler demangler(StringRef(MangledName, MangledNameLength));
  demangler.demangle();
  return demangler.getDemangled();
}

static NodePointer
demangleTypeAsNode(const char *MangledName,
                   size_t MangledNameLength,
                   const DemangleOptions &Options) {
  Demangler demangler(StringRef(MangledName, MangledNameLength));
  demangler.demangleTypeName();
  return demangler.getDemangled();
}

namespace {
class NodePrinter {
private:
  DemanglerPrinter Printer;
  DemangleOptions Options;
  
public:
  NodePrinter(DemangleOptions options) : Options(options) {}
  
  std::string printRoot(NodePointer root) {
    print(root);
    return Printer.str();
  }

private:  
  void printChildren(Node::iterator begin,
                     Node::iterator end,
                     const char *sep = nullptr) {
    for (; begin != end;) {
      print(*begin);
      ++begin;
      if (sep && begin != end)
        Printer << sep;
    }
  }
  
  void printChildren(NodePointer pointer, const char *sep = nullptr) {
    if (!pointer)
      return;
    Node::iterator begin = pointer->begin(), end = pointer->end();
    printChildren(begin, end, sep);
  }
  
  NodePointer getFirstChildOfKind(NodePointer pointer, Node::Kind kind) {
    if (!pointer)
      return nullptr;
    for (NodePointer &child : *pointer) {
      if (child && child->getKind() == kind)
        return child;
    }
    return nullptr;
  }
  
  bool typeNeedsColonForDecl(NodePointer type) {
    if (!type)
      return false;
    if (!type->hasChildren())
      return false;
    NodePointer child = type->getChild(0);
    Node::Kind child_kind = child->getKind();
    switch (child_kind) {
    case Node::Kind::UncurriedFunctionType:
    case Node::Kind::FunctionType:
      return false;
    case Node::Kind::GenericType:
      return typeNeedsColonForDecl(getFirstChildOfKind(type, Node::Kind::UncurriedFunctionType));
    default:
      return true;
    }
  }
  
  void printBoundGenericNoSugar(NodePointer pointer) {
    if (pointer->getNumChildren() < 2)
      return;
    NodePointer typelist = pointer->getChild(1);
    print(pointer->getChild(0));
    Printer << "<";
    printChildren(typelist, ", ");
    Printer << ">";
  }

  static bool isSwiftModule(NodePointer node) {
    return (node->getKind() == Node::Kind::Module &&
            node->getText() == STDLIB_NAME);
  }

  static bool isIdentifier(NodePointer node, StringRef desired) {
    return (node->getKind() == Node::Kind::Identifier &&
            node->getText() == desired);
  }
  
  enum class SugarType {
    None,
    Optional,
    ImplicitlyUnwrappedOptional,
    Array,
    Dictionary
  };
  
  /// Determine whether this is a "simple" type, from the type-simple
  /// production.
  bool isSimpleType(NodePointer pointer) {
    switch (pointer->getKind()) {
    case Node::Kind::ArchetypeAndProtocol:
    case Node::Kind::ArchetypeRef:
    case Node::Kind::AssociatedTypeRef:
    case Node::Kind::BoundGenericClass:
    case Node::Kind::BoundGenericEnum:
    case Node::Kind::BoundGenericStructure:
    case Node::Kind::BuiltinTypeName:
    case Node::Kind::Class:
    case Node::Kind::DependentGenericType:
    case Node::Kind::DependentMemberType:
    case Node::Kind::DependentGenericParamType:
    case Node::Kind::DynamicSelf:
    case Node::Kind::Enum:
    case Node::Kind::ErrorType:
    case Node::Kind::ExistentialMetatype:
    case Node::Kind::Metatype:
    case Node::Kind::Module:
    case Node::Kind::NonVariadicTuple:
    case Node::Kind::Protocol:
    case Node::Kind::QualifiedArchetype:
    case Node::Kind::ReturnType:
    case Node::Kind::SelfTypeRef:
    case Node::Kind::Structure:
    case Node::Kind::TupleElementName:
    case Node::Kind::TupleElementType:
    case Node::Kind::Type:
    case Node::Kind::TypeAlias:
    case Node::Kind::TypeList:
    case Node::Kind::VariadicTuple:
      return true;

    case Node::Kind::Failure:
    case Node::Kind::Addressor:
    case Node::Kind::Allocator:
    case Node::Kind::ArgumentTuple:
    case Node::Kind::AutoClosureType:
    case Node::Kind::Constructor:
    case Node::Kind::Deallocator:
    case Node::Kind::DeclContext:
    case Node::Kind::DefaultArgumentInitializer:
    case Node::Kind::DependentGenericSignature:
    case Node::Kind::DependentGenericParamCount:
    case Node::Kind::DependentGenericConformanceRequirement:
    case Node::Kind::DependentGenericSameTypeRequirement:
    case Node::Kind::DependentProtocolWitnessTableGenerator:
    case Node::Kind::DependentProtocolWitnessTableTemplate:
    case Node::Kind::Destructor:
    case Node::Kind::DidSet:
    case Node::Kind::Directness:
    case Node::Kind::DynamicAttribute:
    case Node::Kind::ExplicitClosure:
    case Node::Kind::Extension:
    case Node::Kind::FieldOffset:
    case Node::Kind::Function:
    case Node::Kind::FunctionType:
    case Node::Kind::Generics:
    case Node::Kind::GenericType:
    case Node::Kind::GenericTypeMetadataPattern:
    case Node::Kind::Getter:
    case Node::Kind::Global:
    case Node::Kind::GlobalGetter:
    case Node::Kind::Identifier:
    case Node::Kind::IVarInitializer:
    case Node::Kind::IVarDestroyer:
    case Node::Kind::ImplConvention:
    case Node::Kind::ImplFunctionAttribute:
    case Node::Kind::ImplFunctionType:
    case Node::Kind::ImplicitClosure:
    case Node::Kind::ImplParameter:
    case Node::Kind::ImplResult:
    case Node::Kind::InOut:
    case Node::Kind::InfixOperator:
    case Node::Kind::Initializer:
    case Node::Kind::LazyProtocolWitnessTableAccessor:
    case Node::Kind::LazyProtocolWitnessTableTemplate:
    case Node::Kind::LocalDeclName:
    case Node::Kind::PrivateDeclName:
    case Node::Kind::MaterializeForSet:
    case Node::Kind::Metaclass:
    case Node::Kind::MutableAddressor:
    case Node::Kind::NominalTypeDescriptor:
    case Node::Kind::NonObjCAttribute:
    case Node::Kind::Number:
    case Node::Kind::ObjCAttribute:
    case Node::Kind::ObjCBlock:
    case Node::Kind::PartialApplyForwarder:
    case Node::Kind::PartialApplyObjCForwarder:
    case Node::Kind::PostfixOperator:
    case Node::Kind::PrefixOperator:
    case Node::Kind::ProtocolConformance:
    case Node::Kind::ProtocolList:
    case Node::Kind::ProtocolWitness:
    case Node::Kind::ProtocolWitnessTable:
    case Node::Kind::ReabstractionThunk:
    case Node::Kind::ReabstractionThunkHelper:
    case Node::Kind::Setter:
    case Node::Kind::SpecializedAttribute:
    case Node::Kind::SpecializationKind:
    case Node::Kind::GenericSpecializationParam:
    case Node::Kind::FunctionSignatureSpecializationParam:
    case Node::Kind::FunctionSignatureSpecializationParamInfo:
    case Node::Kind::Subscript:
    case Node::Kind::Suffix:
    case Node::Kind::TupleElement:
    case Node::Kind::TypeMetadata:
    case Node::Kind::TypeMetadataAccessFunction:
    case Node::Kind::TypeMetadataLazyCache:
    case Node::Kind::UncurriedFunctionType:
    case Node::Kind::Unknown:
    case Node::Kind::Unmanaged:
    case Node::Kind::Unowned:
    case Node::Kind::ValueWitness:
    case Node::Kind::ValueWitnessTable:
    case Node::Kind::Variable:
    case Node::Kind::Weak:
    case Node::Kind::WillSet:
    case Node::Kind::WitnessTableOffset:
      return false;
    }
    unreachable("bad node kind");
  }

  SugarType findSugar(NodePointer pointer) {
    if (pointer->getNumChildren() == 1 && 
        pointer->getKind() == Node::Kind::Type)
      return findSugar(pointer->getChild(0));
    
    if (pointer->getNumChildren() != 2)
      return SugarType::None;
    
    if (pointer->getKind() != Node::Kind::BoundGenericEnum &&
        pointer->getKind() != Node::Kind::BoundGenericStructure)
      return SugarType::None;

    auto unboundType = pointer->getChild(0)->getChild(0); // drill through Type
    auto typeArgs = pointer->getChild(1);
    
    if (pointer->getKind() == Node::Kind::BoundGenericEnum) {
      // Swift.Optional
      if (isIdentifier(unboundType->getChild(1), "Optional") &&
          typeArgs->getNumChildren() == 1 &&
          isSwiftModule(unboundType->getChild(0))) {
        return SugarType::Optional;
      }

      // Swift.ImplicitlyUnwrappedOptional
      if (isIdentifier(unboundType->getChild(1), 
                       "ImplicitlyUnwrappedOptional") &&
          typeArgs->getNumChildren() == 1 &&
          isSwiftModule(unboundType->getChild(0))) {
        return SugarType::ImplicitlyUnwrappedOptional;
      }

      return SugarType::None;
    }

    assert(pointer->getKind() == Node::Kind::BoundGenericStructure);

    // Array
    if (isIdentifier(unboundType->getChild(1), "Array") &&
        typeArgs->getNumChildren() == 1 &&
        isSwiftModule(unboundType->getChild(0))) {
      return SugarType::Array;
    }

    // Dictionary
    if (isIdentifier(unboundType->getChild(1), "Dictionary") &&
        typeArgs->getNumChildren() == 2 &&
        isSwiftModule(unboundType->getChild(0))) {
      return SugarType::Dictionary;
    }

    return SugarType::None;
  }
  
  void printBoundGeneric(NodePointer pointer) {
    if (pointer->getNumChildren() < 2)
      return;
    if (pointer->getNumChildren() != 2) {
      printBoundGenericNoSugar(pointer);
      return;
    }

    if (Options.SynthesizeSugarOnTypes == false ||
        pointer->getKind() == Node::Kind::BoundGenericClass)
    {
      // no sugar here
      printBoundGenericNoSugar(pointer);
      return;
    }

    SugarType sugarType = findSugar(pointer);
    
    switch (sugarType) {
      case SugarType::None:
        printBoundGenericNoSugar(pointer);
        break;
      case SugarType::Optional:
      case SugarType::ImplicitlyUnwrappedOptional: {
        NodePointer type = pointer->getChild(1)->getChild(0);
        bool needs_parens = !isSimpleType(type);
        if (needs_parens)
          Printer << "(";
        print(type);
        if (needs_parens)
          Printer << ")";
        Printer << (sugarType == SugarType::Optional ? "?" : "!");
        break;
      }
      case SugarType::Array: {
        NodePointer type = pointer->getChild(1)->getChild(0);
        Printer << "[";
        print(type);
        Printer << "]";
        break;
      }
      case SugarType::Dictionary: {
        NodePointer keyType = pointer->getChild(1)->getChild(0);
        NodePointer valueType = pointer->getChild(1)->getChild(1);
        Printer << "[";
        print(keyType);
        Printer << " : ";
        print(valueType);
        Printer << "]";
        break;
      }
    }
  }

  void printImplFunctionType(NodePointer fn) {
    enum State { Attrs, Inputs, Results } curState = Attrs;
    auto transitionTo = [&](State newState) {
      assert(newState >= curState);
      for (; curState != newState; curState = State(curState + 1)) {
        switch (curState) {
        case Attrs: Printer << '('; continue;
        case Inputs: Printer << ") -> ("; continue;
        case Results: unreachable("no state after Results");
        }
        unreachable("bad state");
      }
    };

    for (auto &child : *fn) {
      if (child->getKind() == Node::Kind::ImplParameter) {
        if (curState == Inputs) Printer << ", ";
        transitionTo(Inputs);
        print(child);
      } else if (child->getKind() == Node::Kind::ImplResult) {
        if (curState == Results) Printer << ", ";
        transitionTo(Results);
        print(child);
      } else {
        assert(curState == Attrs);
        print(child);
        Printer << ' ';
      }
    }
    transitionTo(Results);
    Printer << ')';
  }

  void printContext(NodePointer context) {
    // TODO: parenthesize local contexts?
    print(context, /*asContext*/ true);
    Printer << '.';
  }

  void print(NodePointer pointer, bool asContext = false, bool suppressType = false);
};
} // end anonymous namespace

static bool isExistentialType(NodePointer node) {
  assert(node->getKind() == Node::Kind::Type);
  node = node->getChild(0);
  return (node->getKind() == Node::Kind::ExistentialMetatype ||
          node->getKind() == Node::Kind::ProtocolList);
}

void NodePrinter::print(NodePointer pointer, bool asContext, bool suppressType) {
  // Common code for handling entities.
  auto printEntity = [&](bool hasName, bool hasType, StringRef extraName) {
    printContext(pointer->getChild(0));

    bool printType = (hasType && !suppressType);
    bool useParens = (printType && asContext);

    if (useParens) Printer << '(';

    if (hasName) print(pointer->getChild(1));
    Printer << extraName;

    if (printType) {
      NodePointer type = pointer->getChild(1 + unsigned(hasName));
      if (typeNeedsColonForDecl(type))
        Printer << " : ";
      else
        Printer << " ";
      print(type);
    }

    if (useParens) Printer << ')';      
  };

  Node::Kind kind = pointer->getKind();
  switch (kind) {
  case Node::Kind::Failure:
    return;
  case Node::Kind::Directness:
    Printer << pointer->getText() << " ";
    return;
  case Node::Kind::Extension:
    assert(pointer->getNumChildren() == 2 && "Extension expects 2 children.");
    Printer << "ext.";
    // Print the module where extension is defined.
    print(pointer->getChild(0), true); 
    Printer << ".";
    print(pointer->getChild(1), asContext);
    return;
  case Node::Kind::Variable:
  case Node::Kind::Function:
  case Node::Kind::Subscript:
    printEntity(true, true, "");
    return;
  case Node::Kind::ExplicitClosure:
  case Node::Kind::ImplicitClosure: {
    auto index = pointer->getChild(1)->getIndex();
    DemanglerPrinter name;
    name << '(';
    if (pointer->getKind() == Node::Kind::ImplicitClosure)
      name << "implicit ";
    name << "closure #" << (index + 1) << ")";
    printEntity(false, false, name.str());
    return;
  }
  case Node::Kind::Global:
    printChildren(pointer);
    return;
  case Node::Kind::Suffix:
    Printer << " with unmangled suffix " << QuotedString(pointer->getText());
    return;
  case Node::Kind::Initializer:
    printEntity(false, false, "(variable initialization expression)");
    return;
  case Node::Kind::DefaultArgumentInitializer: {
    auto index = pointer->getChild(1);
    DemanglerPrinter strPrinter;
    strPrinter << "(default argument " << index->getIndex() << ")";
    printEntity(false, false, strPrinter.str());
    return;
  }
  case Node::Kind::DeclContext:
    print(pointer->getChild(0), asContext);
    return;
  case Node::Kind::Type:
    print(pointer->getChild(0), asContext);
    return;
  case Node::Kind::Class:
  case Node::Kind::Structure:
  case Node::Kind::Enum:
  case Node::Kind::Protocol:
  case Node::Kind::TypeAlias:
    printEntity(true, false, "");
    return;
  case Node::Kind::LocalDeclName:
    Printer << '(';
    print(pointer->getChild(1));
    Printer << " #" << (pointer->getChild(0)->getIndex() + 1) << ')';
    return;
  case Node::Kind::PrivateDeclName:
    Printer << '(';
    print(pointer->getChild(1));
    Printer << " in " << pointer->getChild(0)->getText() << ')';
    return;
  case Node::Kind::Module:
  case Node::Kind::Identifier:
    Printer << pointer->getText();
    return;
  case Node::Kind::AutoClosureType:
    Printer << "@autoclosure ";
    printChildren(pointer);
    return;
  case Node::Kind::FunctionType:
    printChildren(pointer);
    return;
  case Node::Kind::UncurriedFunctionType: {
    NodePointer metatype = pointer->getChild(0);
    Printer << "(";
    print(metatype);
    Printer << ")";
    NodePointer real_func = pointer->getChild(1);
    real_func = real_func->getChild(0);
    printChildren(real_func);
    return;
  }
  case Node::Kind::ArgumentTuple: {
    bool need_parens = false;
    if (pointer->getNumChildren() > 1)
      need_parens = true;
    else {
      if (!pointer->hasChildren())
        need_parens = true;
      else {
        Node::Kind child0_kind = pointer->getChild(0)->getChild(0)->getKind();
        if (child0_kind != Node::Kind::VariadicTuple &&
            child0_kind != Node::Kind::NonVariadicTuple)
          need_parens = true;
      }
    }
    if (need_parens)
      Printer << "(";
    printChildren(pointer);
    if (need_parens)
      Printer << ")";
    return;
  }
  case Node::Kind::NonVariadicTuple:
  case Node::Kind::VariadicTuple: {
    Printer << "(";
    printChildren(pointer, ", ");
    if (pointer->getKind() == Node::Kind::VariadicTuple)
      Printer << "...";
    Printer << ")";
    return;
  }
  case Node::Kind::TupleElement:
    if (pointer->getNumChildren() == 1) {
      NodePointer type = pointer->getChild(0);
      print(type);
    } else if (pointer->getNumChildren() == 2) {
      NodePointer id = pointer->getChild(0);
      NodePointer type = pointer->getChild(1);
      print(id);
      print(type);
    }
    return;
  case Node::Kind::TupleElementName:
    Printer << pointer->getText() << " : ";
    return;
  case Node::Kind::TupleElementType:
    Printer << pointer->getText();
    return;
  case Node::Kind::ReturnType:
    if (pointer->getNumChildren() == 0)
      Printer << " -> " << pointer->getText();
    else {
      Printer << " -> ";
      printChildren(pointer);
    }
    return;
  case Node::Kind::Weak:
    Printer << "weak ";
    print(pointer->getChild(0));
    return;
  case Node::Kind::Unowned:
    Printer << "unowned ";
    print(pointer->getChild(0));
    return;
  case Node::Kind::Unmanaged:
    Printer << "unowned(unsafe) ";
    print(pointer->getChild(0));
    return;
  case Node::Kind::InOut:
    Printer << "inout ";
    print(pointer->getChild(0));
    return;
  case Node::Kind::NonObjCAttribute:
    Printer << "@!objc ";
    return;
  case Node::Kind::ObjCAttribute:
    Printer << "@objc ";
    return;
  case Node::Kind::DynamicAttribute:
    Printer << "dynamic ";
    return;
  case Node::Kind::SpecializedAttribute:
    print(pointer->getChild(0));
    Printer << " specialization <";
    for (unsigned i = 1, e = pointer->getNumChildren(); i < e; ++i) {
      if (i >= 2)
        Printer << ", ";
      print(pointer->getChild(i));
    }
    Printer << "> of ";
    return;
  case Node::Kind::SpecializationKind:
    Printer << pointer->getText();
    return;
  case Node::Kind::GenericSpecializationParam:
    print(pointer->getChild(0));
    for (unsigned i = 1, e = pointer->getNumChildren(); i < e; ++i) {
      if (i == 1)
        Printer << " with ";
      else
        Printer << " and ";
      print(pointer->getChild(i));
    }
    return;
  case Node::Kind::FunctionSignatureSpecializationParam: {
    uint64_t argNum = pointer->getIndex();

    Printer << "Arg[" << argNum << "] = ";
    print(pointer->getChild(0));
    for (unsigned i = 1, e = pointer->getNumChildren(); i < e; ++i) {
      Printer << " and ";
      print(pointer->getChild(i));
    }
    return;
  }
  case Node::Kind::FunctionSignatureSpecializationParamInfo: {
    uint64_t rawKind = pointer->getIndex();
    switch (FunctionSigSpecializationParamInfoKind(rawKind)) {
    case FunctionSigSpecializationParamInfoKind::ConstantPropFunction:
    case FunctionSigSpecializationParamInfoKind::ConstantPropGlobal:
    case FunctionSigSpecializationParamInfoKind::ConstantPropInteger:
    case FunctionSigSpecializationParamInfoKind::ConstantPropFloat:
    case FunctionSigSpecializationParamInfoKind::ConstantPropString:
      Printer << "Constant Propagated";
      break;
    case FunctionSigSpecializationParamInfoKind::ClosureProp:
      Printer << "Closure Propagated";
      break;
    }
    return;
  }
  case Node::Kind::BuiltinTypeName:
    Printer << pointer->getText();
    return;
  case Node::Kind::Number:
    Printer << pointer->getIndex();
    return;
  case Node::Kind::InfixOperator:
    Printer << pointer->getText() << " infix";
    return;
  case Node::Kind::PrefixOperator:
    Printer << pointer->getText() << " prefix";
    return;
  case Node::Kind::PostfixOperator:
    Printer << pointer->getText() << " postfix";
    return;
  case Node::Kind::DependentProtocolWitnessTableGenerator:
    Printer << "dependent protocol witness table generator for ";
    print(pointer->getFirstChild());
    return;
  case Node::Kind::DependentProtocolWitnessTableTemplate:
    Printer << "dependent protocol witness table template for ";
    print(pointer->getFirstChild());
    return;
  case Node::Kind::LazyProtocolWitnessTableAccessor:
    Printer << "lazy protocol witness table accessor for ";
    print(pointer->getFirstChild());
    return;
  case Node::Kind::LazyProtocolWitnessTableTemplate:
    Printer << "lazy protocol witness table template for ";
    print(pointer->getFirstChild());
    return;
  case Node::Kind::ProtocolWitnessTable:
    Printer << "protocol witness table for ";
    print(pointer->getFirstChild());
    return;
  case Node::Kind::ProtocolWitness: {
    Printer << "protocol witness for ";
    print(pointer->getChild(1));
    Printer << " in conformance ";
    print(pointer->getChild(0));
    return;
  }
  case Node::Kind::PartialApplyForwarder:
    Printer << "partial apply forwarder";
    if (pointer->hasChildren()) {
      Printer << " for ";
      print(pointer->getFirstChild());
    }
    return;
  case Node::Kind::PartialApplyObjCForwarder:
    Printer << "partial apply ObjC forwarder";
    if (pointer->hasChildren()) {
      Printer << " for ";
      print(pointer->getFirstChild());
    }
    return;
  case Node::Kind::FieldOffset: {
    print(pointer->getChild(0)); // directness
    Printer << "field offset for ";
    auto entity = pointer->getChild(1);
    print(entity, /*asContext*/ false,
             /*suppressType*/ !Options.DisplayTypeOfIVarFieldOffset);
    return;
  }
  case Node::Kind::ReabstractionThunk:
  case Node::Kind::ReabstractionThunkHelper: {
    Printer << "reabstraction thunk ";
    if (pointer->getKind() == Node::Kind::ReabstractionThunkHelper)
      Printer << "helper ";
    auto generics = getFirstChildOfKind(pointer, Node::Kind::DependentGenericSignature);
    assert(pointer->getNumChildren() == 2 + unsigned(generics != nullptr));
    if (generics) {
      print(generics);
      Printer << " ";
    }
    Printer << "from ";
    print(pointer->getChild(pointer->getNumChildren() - 2));
    Printer << " to ";
    print(pointer->getChild(pointer->getNumChildren() - 1));
    return;
  }
  case Node::Kind::GenericTypeMetadataPattern:
    print(pointer->getChild(0)); // directness
    Printer << "generic type metadata pattern for ";
    print(pointer->getChild(1));
    return;
  case Node::Kind::Metaclass:
    Printer << "metaclass for ";
    print(pointer->getFirstChild());
    return;
  case Node::Kind::TypeMetadata:
    print(pointer->getChild(0)); // directness
    Printer << "type metadata for ";
    print(pointer->getChild(1));
    return;
  case Node::Kind::TypeMetadataAccessFunction:
    Printer << "type metadata accessor for ";
    print(pointer->getChild(0));
    return;
  case Node::Kind::TypeMetadataLazyCache:
    Printer << "lazy cache variable for type metadata for ";
    print(pointer->getChild(0));
    return;
  case Node::Kind::NominalTypeDescriptor:
    Printer << "nominal type descriptor for ";
    print(pointer->getChild(0));
    return;
  case Node::Kind::ValueWitness:
    Printer << pointer->getText() << " value witness for ";
    print(pointer->getFirstChild());
    return;
  case Node::Kind::ValueWitnessTable:
    Printer << "value witness table for ";
    print(pointer->getFirstChild());
    return;
  case Node::Kind::WitnessTableOffset:
    Printer << "witness table offset for ";
    print(pointer->getFirstChild());
    return;
  case Node::Kind::BoundGenericClass:
  case Node::Kind::BoundGenericStructure:
  case Node::Kind::BoundGenericEnum:
    printBoundGeneric(pointer);
    return;
  case Node::Kind::DynamicSelf:
    Printer << "Self";
    return;
  case Node::Kind::ObjCBlock: {
    Printer << "@objc_block ";
    NodePointer tuple = pointer->getChild(0);
    NodePointer rettype = pointer->getChild(1);
    print(tuple);
    print(rettype);
    return;
  }
  case Node::Kind::Metatype: {
    NodePointer type = pointer->getChild(0);
    print(type);
    if (isExistentialType(type)) {
      Printer << ".Protocol";
    } else {
      Printer << ".Type";
    }
    return;
  }
  case Node::Kind::ExistentialMetatype: {
    NodePointer type = pointer->getChild(0);
    print(type);
    Printer << ".Type";
    return;
  }
  case Node::Kind::ArchetypeRef:
    Printer << pointer->getText();
    return;
  case Node::Kind::AssociatedTypeRef:
    print(pointer->getChild(0));
    Printer << '.' << pointer->getChild(1)->getText();
    return;
  case Node::Kind::SelfTypeRef:
    print(pointer->getChild(0));
    Printer << ".Self";
    return;
  case Node::Kind::ProtocolList: {
    NodePointer type_list = pointer->getChild(0);
    if (!type_list)
      return;
    bool needs_proto_marker = (type_list->getNumChildren() != 1);
    if (needs_proto_marker)
      Printer << "protocol<";
    printChildren(type_list, ", ");
    if (needs_proto_marker)
      Printer << ">";
    return;
  }
  case Node::Kind::Generics: {
    if (pointer->getNumChildren() == 0)
      return;
    Printer << "<";
    printChildren(pointer, ", ");
    Printer << ">";
    return;
  }
  case Node::Kind::QualifiedArchetype: {
    if (pointer->getNumChildren() < 2)
      return;
    NodePointer number = pointer->getChild(0);
    NodePointer decl_ctx = pointer->getChild(1);
    Printer << "(archetype " << number->getIndex() << " of ";
    print(decl_ctx);
    Printer << ")";
    return;
  }
  case Node::Kind::GenericType: {
    NodePointer atype_list = pointer->getChild(0);
    NodePointer fct_type = pointer->getChild(1)->getChild(0);
    print(atype_list);
    print(fct_type);
    return;
  }
  case Node::Kind::Addressor:
    printEntity(true, true, ".addressor");
    return;
  case Node::Kind::MutableAddressor:
    printEntity(true, true, ".mutableAddressor");
    return;
  case Node::Kind::GlobalGetter:
    printEntity(true, true, ".getter");
    return;
  case Node::Kind::Getter:
    printEntity(true, true, ".getter");
    return;
  case Node::Kind::Setter:
    printEntity(true, true, ".setter");
    return;
  case Node::Kind::MaterializeForSet:
    printEntity(true, true, ".materializeForSet");
    return;
  case Node::Kind::WillSet:
    printEntity(true, true, ".willset");
    return;
  case Node::Kind::DidSet:
    printEntity(true, true, ".didset");
    return;
  case Node::Kind::Allocator:
    printEntity(false, true, "__allocating_init");
    return;
  case Node::Kind::Constructor:
    printEntity(false, true, "init");
    return;
  case Node::Kind::Destructor:
    printEntity(false, false, "deinit");
    return;
  case Node::Kind::Deallocator:
    printEntity(false, false, "__deallocating_deinit");
    return;
  case Node::Kind::IVarInitializer:
    printEntity(false, false, "__ivar_initializer");
    return;
  case Node::Kind::IVarDestroyer:
    printEntity(false, false, "__ivar_destroyer");
    return;
  case Node::Kind::ProtocolConformance: {
    NodePointer child0 = pointer->getChild(0);
    NodePointer child1 = pointer->getChild(1);
    NodePointer child2 = pointer->getChild(2);
    print(child0);
    Printer << " : ";
    print(child1);
    Printer << " in ";
    print(child2);
    return;
  }
  case Node::Kind::TypeList:
    printChildren(pointer);
    return;
  case Node::Kind::ArchetypeAndProtocol: {
    NodePointer child0 = pointer->getChild(0);
    NodePointer child1 = pointer->getChild(1);
    print(child0);
    Printer << " : ";
    print(child1);
    return;
  }
  case Node::Kind::ImplConvention:
    Printer << pointer->getText();
    return;
  case Node::Kind::ImplFunctionAttribute:
    Printer << pointer->getText();
    return;
  case Node::Kind::ImplParameter:
  case Node::Kind::ImplResult:
    printChildren(pointer, " ");
    return;
  case Node::Kind::ImplFunctionType:
    printImplFunctionType(pointer);
    return;
  case Node::Kind::Unknown:
    return;
  case Node::Kind::ErrorType:
    Printer << "<ERROR TYPE>";
    return;
      
  case Node::Kind::DependentGenericSignature: {
    Printer << '<';
    
    unsigned depth = 0;
    unsigned numChildren = pointer->getNumChildren();
    for (;
         depth < numChildren
           && pointer->getChild(depth)->getKind()
               == Node::Kind::DependentGenericParamCount;
         ++depth) {
      unsigned count = pointer->getChild(depth)->getIndex();
      for (unsigned index = 0; index < count; ++index) {
        if (depth || index)
          Printer << ", ";
        Printer << "T_" << depth << '_' << index;
      }
    }
    
    if (depth != numChildren) {
      Printer << " where ";
      for (unsigned i = depth; i < numChildren; ++i) {
        if (i > depth)
          Printer << ", ";
        print(pointer->getChild(i));
      }
    }
    Printer << '>';
    return;
  }
  case Node::Kind::DependentGenericParamCount:
    unreachable("should be printed as a child of a "
                "DependentGenericSignature");
  case Node::Kind::DependentGenericConformanceRequirement: {
    NodePointer type = pointer->getChild(0);
    NodePointer reqt = pointer->getChild(1);
    print(type);
    Printer << ": ";
    print(reqt);
    return;
  }
  case Node::Kind::DependentGenericSameTypeRequirement: {
    NodePointer fst = pointer->getChild(0);
    NodePointer snd = pointer->getChild(1);
    
    print(fst);
    Printer << " == ";
    print(snd);
    return;
  }
  case Node::Kind::DependentGenericParamType: {
    Printer << pointer->getText();
    return;
  }
  case Node::Kind::DependentGenericType: {
    NodePointer sig = pointer->getChild(0);
    NodePointer depTy = pointer->getChild(1);
    print(sig);
    Printer << ' ';
    print(depTy);
    return;
  }
  case Node::Kind::DependentMemberType: {
    NodePointer base = pointer->getChild(0);
    print(base);
    Printer << '.' << pointer->getText();
    return;
  }
  }
  unreachable("bad node kind!");
}

std::string Demangle::nodeToString(NodePointer root,
                                   const DemangleOptions &options) {
  if (!root)
    return "";

  return NodePrinter(options).printRoot(root);
}

std::string Demangle::demangleSymbolAsString(const char *MangledName,
                                             size_t MangledNameLength,
                                             const DemangleOptions &Options) {
  auto mangled = StringRef(MangledName, MangledNameLength);
  auto root = demangleSymbolAsNode(MangledName, MangledNameLength, Options);
  if (!root) return mangled.str();

  std::string demangling = nodeToString(std::move(root), Options);
  if (demangling.empty())
    return mangled.str();
  return demangling;
}

std::string Demangle::demangleTypeAsString(const char *MangledName,
                                           size_t MangledNameLength,
                                           const DemangleOptions &Options) {
  auto mangled = StringRef(MangledName, MangledNameLength);
  auto root = demangleTypeAsNode(MangledName, MangledNameLength, Options);
  if (!root) return mangled.str();
  
  std::string demangling = nodeToString(std::move(root), Options);
  if (demangling.empty())
    return mangled.str();
  return demangling;
}
