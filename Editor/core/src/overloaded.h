#pragma once

namespace end_editor {

    // A visitor made of lambdas, one per alternative: std::visit(Overloaded{[](const A&) {...},
    // [](const B&) {...}}, value).
    template<class... Ts>
    struct Overloaded : Ts... {
        using Ts::operator()...;
    };
    template<class... Ts>
    Overloaded(Ts...) -> Overloaded<Ts...>;
}
