#pragma once

namespace qtidm::automation {

// The one place the automation channel's names live, so the service, the
// discovery file and the command-line client cannot drift apart.
inline constexpr auto service = "io.qtidm.Qtidm";
inline constexpr auto objectPath = "/io/qtidm/Qtidm/Application";
inline constexpr auto interface = "io.qtidm.Qtidm.Application";
inline constexpr auto fileName = "ipc.json";

}
