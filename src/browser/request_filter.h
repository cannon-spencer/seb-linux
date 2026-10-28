#pragma once
#include "../seb_settings.h"

#include <QRegularExpression>
#include <QVector>

#include "contracts/i_request_interceptor.h"

#include <QtGlobal>
#include <QUrl>

namespace seb::browser {

enum class FilterDecision
{
    NoMatch,
    Allow,
    Block
};

class RequestFilter
{
public:
    explicit RequestFilter(const seb::FilterSettings &settings);

    FilterDecision evaluate(const QUrl &url, contracts::ResourceType resourceType) const;

    // Regex source a rule is matched with (case-insensitively). Used to hand
    // the same rules to the WebKitGTK web process extension.
    static QString patternFor(const seb::FilterRuleSettings &rule);

private:
    struct CompiledRule
    {
        QRegularExpression expression;
        seb::FilterResult result = seb::FilterResult::Block;
    };

    static QRegularExpression compileRule(const seb::FilterRuleSettings &rule);
    static bool isMainRequest(contracts::ResourceType resourceType);

    seb::FilterSettings settings_;
    QVector<CompiledRule> allowRules_;
    QVector<CompiledRule> blockRules_;
};

}  // namespace seb::browser
