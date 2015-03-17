
#ifndef _AST__PATTERN_HPP_INCLUDED_
#define _AST__PATTERN_HPP_INCLUDED_

#include <vector>
#include <memory>
#include <string>
#include <tagged_enum.hpp>

namespace AST {

using ::std::unique_ptr;
using ::std::move;

class ExprNode;

class Pattern:
    public Serialisable
{
public:
    enum BindType {
        MAYBE_BIND,
        ANY,
        REF,
        VALUE,
        TUPLE,
        TUPLE_STRUCT,
    };
private:
    BindType    m_class;
    ::std::string   m_binding;
    Path    m_path;
    unique_ptr<ExprNode>    m_node;
    unique_ptr<ExprNode>    m_node2;    // ONLY used for range values
    ::std::vector<Pattern>  m_sub_patterns;
public:
    Pattern(Pattern&& o) noexcept:
        m_class(o.m_class),
        m_binding( move(o.m_binding) ),
        m_path( move(o.m_path) ),
        m_node( move(o.m_node) ),
        m_sub_patterns( move(o.m_sub_patterns) )
    { }
    
    Pattern(const Pattern& o):
        m_class(o.m_class),
        m_binding(o.m_binding),
        m_path(o.m_path),
        m_node(nullptr),
        m_sub_patterns(o.m_sub_patterns)
    {
        if( o.m_node.get() ) {
            DEBUG("Cloning " << o);
            throw ::std::runtime_error(FMT("Cloning pattern with node : " << o));
        }
    }
    
    Pattern& operator=(Pattern o)
    {
        m_class = o.m_class,
        m_binding = move(o.m_binding);
        m_path = move(o.m_path);
        m_node = move(o.m_node);
        m_sub_patterns = move(o.m_sub_patterns);
        return *this;
    }
    
    Pattern():
        m_class(ANY)
    {}

    struct TagBind {};
    Pattern(TagBind, ::std::string name):
        m_class(ANY),
        m_binding(name)
    {}

    struct TagMaybeBind {};
    Pattern(TagMaybeBind, ::std::string name):
        m_class(MAYBE_BIND),
        m_binding(name)
    {}

    struct TagValue {};
    Pattern(TagValue, unique_ptr<ExprNode> node, unique_ptr<ExprNode> node2 = 0):
        m_class(VALUE),
        m_node( ::std::move(node) ),
        m_node2( ::std::move(node2) )
    {}
    
    
    struct TagReference {};
    Pattern(TagReference, Pattern sub_pattern):
        m_class(REF),
        m_sub_patterns()
    {
        m_sub_patterns.push_back( ::std::move(sub_pattern) );
    }

    struct TagTuple {};
    Pattern(TagTuple, ::std::vector<Pattern> sub_patterns):
        m_class(TUPLE),
        m_sub_patterns( ::std::move(sub_patterns) )
    {}

    struct TagEnumVariant {};
    Pattern(TagEnumVariant, Path path, ::std::vector<Pattern> sub_patterns):
        m_class(TUPLE_STRUCT),
        m_path( ::std::move(path) ),
        m_sub_patterns( ::std::move(sub_patterns) )
    {}
    
    // Mutators
    void set_bind(::std::string name, bool is_ref, bool is_mut) {
        m_binding = name;
    }
    
    ::std::unique_ptr<ExprNode> take_node() { assert(m_class == VALUE); m_class = ANY; return ::std::move(m_node); }
    
    // Accessors
    const ::std::string& binding() const { return m_binding; }
    BindType type() const { return m_class; }
    ExprNode& node() { return *m_node; }
    const ExprNode& node() const { return *m_node; }
    Path& path() { return m_path; }
    const Path& path() const { return m_path; }
    ::std::vector<Pattern>& sub_patterns() { return m_sub_patterns; }
    const ::std::vector<Pattern>& sub_patterns() const { return m_sub_patterns; }

    friend ::std::ostream& operator<<(::std::ostream& os, const Pattern& pat);

    SERIALISABLE_PROTOTYPES();
};

};

#endif
