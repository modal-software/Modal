```go
package semantics

import (
	"errors"
	"fmt"
	"path/to/ast"
	"path/to/syntax" // Assuming this is the package for Scope
)

// ResolveFunction checks the scope chain to see if a function matches the call signature.
// Returns the resolved function definition or an error.
func ResolveFunction(s *syntax.Scope, callExpr *ast.CallExpr) (*syntax.FunctionDefinition, error) {
	identName := callExpr.GetIdentifierName() // Assumes CallExpr stores the identifier name
	
	// 1. Scope Lookup (Check Existence)
	if !s.IsFunctionDefined(identName) {
		return nil, errors.New("Function not found: " + identName + " is not declared in this scope")
	}

	// 2. Signature Matching
	var potentialMatches []syntax.FunctionDefinition
	
	// Traverse the scope chain to find all matching definitions
	currentScope := s
	for currentScope != nil {
		if fnDef, exists := currentScope.Functions[identName]; exists {
			potentialMatches = append(potentialMatches, fnDef)
		}
		currentScope = currentScope.Parent
	}
	
	if len(potentialMatches) == 0 {
		// Should be caught by IsFunctionDefined, but this is a safety check
		return nil, errors.New("Internal semantic error: no functions found for name " + identName)
	}

	// Check for ambiguity (Multiple overloads)
	// A robust implementation would compare argument types here.
	// For this minimal version, we assume simple signature uniqueness.
	if len(potentialMatches) > 1 {
		return nil, fmt.Errorf("Ambiguous function call: multiple definitions found for %s. Cannot resolve signature", identName)
	}

	// 3. Return the unique match
	return &potentialMatches[0], nil
}
```
