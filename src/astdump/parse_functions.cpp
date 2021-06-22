// Copyright (C) 2017-2021 Jonathan Müller <jonathanmueller.dev@gmail.com>
// This file is subject to the license terms in the LICENSE file
// found in the top-level directory of this distribution.

#include "parse_functions.hpp"

using namespace cppast;

namespace
{
astdump_detail::dom::object get_actual_location(astdump_detail::dom::object&& location)
{
    auto expansion_loc = location["expansionLoc"];
    if (expansion_loc.error() != simdjson::error_code::NO_SUCH_FIELD)
        // Declaration was generated by a macro, actual location is this sub field.
        location = expansion_loc.value();

    return std::move(location);
}
} // namespace

source_location astdump_detail::get_location(dom::object& entity)
{
    source_location result;

    auto location = get_actual_location(entity["loc"].value());

    auto file = location["file"];
    if (file.error() != simdjson::error_code::NO_SUCH_FIELD)
        result.file = std::string(file.get_string().value());

    auto line = location["line"];
    if (line.error() != simdjson::error_code::NO_SUCH_FIELD)
        result.line = line.get_uint64().value();
    auto col = location["col"];
    if (col.error() != simdjson::error_code::NO_SUCH_FIELD)
        result.column = col.get_uint64().value();

    auto name = entity["name"];
    if (name.error() != simdjson::error_code::NO_SUCH_FIELD)
        result.entity = std::string(name.get_string().value());

    return result;
}

cpp_entity_id astdump_detail::get_entity_id(parse_context& context, dom::object& entity)
{
    // This id is only valid within one translation unit.
    auto tu_id = entity["id"].raw_json_token();

    // We turn it into a global id by prefixing it with the file.
    // TODO: maybe allow cross references by using file + offset as id or something?
    return cpp_entity_id(context.path + std::string(tu_id.value()));
}

std::string astdump_detail::parse_comment(parse_context& context, dom::object entity)
{
    // TODO: C style comments
    std::string result;
    for (dom::object child : entity["inner"])
    {
        if (child["kind"].get_string().value() == "ParagraphComment")
        {
            // Recursively process its children.
            result += parse_comment(context, child);
        }
        else
        {
            if (!result.empty())
                result.push_back('\n');

            auto text = child["text"].get_string().value();
            if (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
                text.remove_prefix(1);
            result.append(text.data(), text.size());
        }
    }
    return result;
}

std::unique_ptr<cpp_entity> astdump_detail::parse_unexposed_entity(parse_context& context,
                                                                   dom::object    entity)
{
    dom::object range = entity["range"];

    auto begin = get_actual_location(range["begin"])["offset"].get_uint64().value();
    auto end   = get_actual_location(range["end"])["offset"].get_uint64().value();
    if (begin > end)
        throw std::invalid_argument("range is invalid");

    std::string tokens(end - begin, '\0');
    context.file.seekg(begin);
    context.file.read(&tokens[0], tokens.size());

    auto spelling = cpp_token_string::tokenize(tokens);

    auto name = entity["name"];
    if (name.error() != simdjson::NO_SUCH_FIELD)
        return cpp_unexposed_entity::build(*context.idx, get_entity_id(context, entity),
                                           std::string(name.get_string().value()),
                                           std::move(spelling));
    else
        return cpp_unexposed_entity::build(std::move(spelling));
}

std::unique_ptr<cpp_entity> astdump_detail::parse_entity(parse_context& context, cpp_entity& parent,
                                                         std::string_view kind, dom::object entity)
try
{
    if (context.logger->is_verbose())
    {
        context.logger->log("astdump parser",
                            format_diagnostic(severity::debug, get_location(entity),
                                              "parsing entity of type '", kind, "'"));
    }

    if (kind == "FullComment")
    {
        auto comment = parse_comment(context, entity);
        parent.set_comment(std::move(comment));
        return nullptr;
    }
    else if (kind == "LinkageSpecDecl")
        return parse_language_linkage(context, entity);
    else if (kind == "NamespaceDecl")
        return parse_namespace(context, entity);

    // Build an unexposed entity.
    return parse_unexposed_entity(context, entity);
}
catch (simdjson::simdjson_error& ex)
{
    context.logger->log("astdump parser",
                        format_diagnostic(severity::error, get_location(entity),
                                          "unexepted JSON for entity: ", ex.what()));
    context.error = true;
    return nullptr;
}
catch (std::logic_error& ex)
{
    context.logger->log("astdump parser",
                        format_diagnostic(severity::error, get_location(entity),
                                          "ill-formed JSON for entity: ", ex.what()));
    context.error = true;
    return nullptr;
}

