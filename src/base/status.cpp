#include "base/status.hpp"

namespace rv {

// Uma linha por código, no mesmo lugar. A tentação de gerar isto por macro existe; resistimos
// porque a lista legível é o que alguém lê às três da manhã com um número na mão.
const char* to_string(Err e) noexcept {
  switch (e) {
    case Err::Ok: return "Ok";
    case Err::OutOfRange: return "OutOfRange";
    case Err::ArenaExhausted: return "ArenaExhausted";
    case Err::Overflow: return "Overflow";
    case Err::NotFound: return "NotFound";
    case Err::WouldBlock: return "WouldBlock";
    case Err::InvalidArgument: return "InvalidArgument";
    case Err::UnknownTemplate: return "UnknownTemplate";
    case Err::ShortPayload: return "ShortPayload";
    case Err::BadBlockLength: return "BadBlockLength";
    case Err::GroupTooLarge: return "GroupTooLarge";
    case Err::Misaligned: return "Misaligned";
    case Err::InvalidTransition: return "InvalidTransition(I5)";
    case Err::AlreadyApplied: return "AlreadyApplied(I6)";
    case Err::NegativeBucket: return "NegativeBucket(I3)";
    case Err::QtyMismatch: return "QtyMismatch";
    case Err::UnknownBatch: return "UnknownBatch";
    case Err::AmountMismatch: return "AmountMismatch";
    case Err::ShortSaleNotAllowed: return "ShortSaleNotAllowed(I3)";
    case Err::UnknownInstrument: return "UnknownInstrument[FATAL]";
    case Err::UnknownAccount: return "UnknownAccount[FATAL]";
    case Err::LedgerOverflow: return "LedgerOverflow[FATAL]";
    case Err::StateCorrupt: return "StateCorrupt[FATAL]";
    case Err::WalFull: return "WalFull";
    case Err::ShortWrite: return "ShortWrite[FATAL]";
    case Err::IoError: return "IoError[FATAL]";
    case Err::BadCrc: return "BadCrc[FATAL]";
    case Err::BadMagic: return "BadMagic[FATAL]";
    case Err::LsnGap: return "LsnGap[FATAL]";
    case Err::EpochMismatch: return "EpochMismatch[FATAL]";
    case Err::SegmentFull: return "SegmentFull[FATAL]";
    case Err::MissingInteractionId: return "MissingInteractionId(R1)";
    case Err::BadSignature: return "BadSignature(R7)";
    case Err::TokenExpired: return "TokenExpired(R2)";
    case Err::CertBindingMismatch: return "CertBindingMismatch(R3)";
    case Err::ConsentNotAuthorised: return "ConsentNotAuthorised(R4)";
    case Err::ScopeMissing: return "ScopeMissing(R5)";
    case Err::OperationalLimit: return "OperationalLimit(R16)";
    case Err::RateLimited: return "RateLimited(R16)";
    case Err::MalformedRequest: return "MalformedRequest";
    case Err::ResourceNotFound: return "ResourceNotFound";
    case Err::TlsProfileViolation: return "TlsProfileViolation(R6)";
  }
  return "Err(?)";
}

}  // namespace rv
