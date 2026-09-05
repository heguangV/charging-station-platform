#include "core/application/admin_repository.h"

namespace ncs::core::application {

std::string roleName(const Role role) {
  switch (role) {
  case Role::User:
    return "USER";
  case Role::Operator:
    return "OPERATOR";
  case Role::Owner:
    return "OWNER";
  case Role::Viewer:
    return "VIEWER";
  case Role::MlWorker:
    return "ML_WORKER";
  case Role::MlTrainer:
    return "ML_TRAINER";
  case Role::MlPredictor:
    return "ML_PREDICTOR";
  }
  return {};
}

} // namespace ncs::core::application
