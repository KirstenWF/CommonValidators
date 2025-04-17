#include "EditorValidator_PureNode.h"

#include "Engine/Blueprint.h"
#include "Misc/DataValidation.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Knot.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_GetSubsystem.h"
#include "K2Node_BreakStruct.h"

bool UEditorValidator_PureNode::CanValidateAsset_Implementation(const FAssetData& InAssetData, UObject* InObject, FDataValidationContext& InContext) const
{
	return InObject && InObject->IsA<UBlueprint>();
}

// WAYFINDER_CHANGE: kirsten@wayfindergames.se, Kauri-XXX - BEGIN: adapt pure node validation to our needs
bool IsMultiPinPureNode(const UK2Node* PureNode)
{
	int PinConnectionCount = 0;
	for (const UEdGraphPin* Pin : PureNode->Pins)
	{
		//if we're an output pin and we have a connection in the graph - this counts.
		if (Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 1)
		{
			return true;
		}
	}

	return false;
}

bool IsArrayOutputUsedAsMacroInput(const UK2Node* PureNode)
{
	int PinConnectionCount = 0;
	for (const UEdGraphPin* Pin : PureNode->Pins)
	{
		//if we're an output pin and we have a connection in the graph - this counts.
		if (Pin->Direction == EGPD_Output && Pin->PinType.IsContainer())
		{
			for (auto Out : Pin->LinkedTo)
			{
				if (Cast<UK2Node_MacroInstance>(Out->GetOwningNode()))
				{
					return true;
				}
			}
		}
	}

	return false;
}

EDataValidationResult ValidateGraph(const UEdGraph* Graph, FDataValidationContext& Context)
{
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		const UK2Node* PureNode = Cast<UK2Node>(Node);
		if (PureNode && PureNode->IsNodePure())
		{
			if (Node->IsA<UK2Node_VariableGet>())
			{
				// kirsten: ignore getters
				continue;
			}

			if (Node->IsA<UK2Node_Self>())
			{
				// kirsten: ignore self
				continue;
			}

			if (Node->IsA<UK2Node_Knot>())
			{
				// kirsten: ignore reroute nodes
				continue;
			}

			if (Node->IsA<UK2Node_CreateDelegate>())
			{
				// kirsten: ignore events
				continue;
			}

			if (Node->IsA<UK2Node_GetSubsystem>())
			{
				// kirsten: ignore subsystems
				continue;
			}

			if (Node->IsA<UK2Node_BreakStruct>())
			{
				// kirsten: ignore breakstructs
				continue;
			}

			if (IsArrayOutputUsedAsMacroInput(PureNode))
			{
				FText output = FText::Join(FText::FromString(" "), PureNode->GetNodeTitle(ENodeTitleType::Type::MenuTitle), FText::FromString(TEXT("We do not allow pure node array outputs as an input for macros.")));
				Context.AddError(output);
				return EDataValidationResult::Invalid;
			}

			auto CallFunctionNode = Cast<UK2Node_CallFunction>(Node);
			if (CallFunctionNode && CallFunctionNode->GetTargetFunction() && CallFunctionNode->GetTargetFunction()->HasAnyFunctionFlags(FUNC_Const | FUNC_Static))
			{
				// kirsten: ignore const functions for now
				continue;
			}

			if (IsMultiPinPureNode(PureNode))
			{
				FText output = FText::Join(FText::FromString(" "), PureNode->GetNodeTitle(ENodeTitleType::Type::MenuTitle), FText::FromString(TEXT("MultiPin Pure Nodes actually get called for each connected pin output.")));
				Context.AddError(output);
				return EDataValidationResult::Invalid;
			}
		}
	}
	return EDataValidationResult::Valid;
}

EDataValidationResult UEditorValidator_PureNode::ValidateLoadedAsset_Implementation(const FAssetData& InAssetData, UObject* InAsset, FDataValidationContext& Context)
{
	UBlueprint* Blueprint = Cast<UBlueprint>(InAsset);
	if (!Blueprint) return EDataValidationResult::NotValidated;

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (ValidateGraph(Graph, Context) == EDataValidationResult::Invalid)
		{
			return EDataValidationResult::Invalid;
		}
	}

	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (ValidateGraph(Graph, Context) == EDataValidationResult::Invalid)
		{
			return EDataValidationResult::Invalid;
		}
	}

	return EDataValidationResult::Valid;
}
// WAYFINDER_CHANGE: kirsten@wayfindergames.se - END