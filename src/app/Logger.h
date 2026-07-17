#pragma once

#include <QString>

namespace qtidm::Logger {

void install();
void info(const QString& context, const QString& message);
void error(const QString& context, const QString& message);

}
