#include "EditorValidator_PureNode.h"

#include "Engine/Blueprint.h"
#include "Misc/DataValidation.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_GetSubsystem.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "CommonValidatorsStatics.h"

#include "K2Node.h"

namespace EditorValidator_PureNode
{
	bool IsWhitelistedPureNode(UK2Node* PureNode)
	{
		static const TArray<UClass*> WhitelistedTypes = {
				UK2Node_BreakStruct::StaticClass(),
				UK2Node_VariableGet::StaticClass(),
				UK2Node_GetSubsystem::StaticClass(),
				UK2Node_CreateDelegate::StaticClass(),
				UK2Node_Knot::StaticClass(),
				UK2Node_Self::StaticClass(),
				//UK2Node_PropertyAccess > ()) // kirsten: it's a private type, I don't want to diverge too much
		};

		static const TArray<FString> WhitelistedTypeNames = {
			 "K2Node_PropertyAccess"
		};

		if (WhitelistedTypeNames.Contains(PureNode->GetClass()->GetName()))
		{
			return true;
		}

		for (UClass* WhitelistedClass : WhitelistedTypes)
		{
			if (PureNode->IsA(WhitelistedClass))
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

// WAYFINDER_CHANGE: kirsten@wayfindergames.se - BEGIN: adapt pure node validation to our needs
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
// WAYFINDER_CHANGE: kirsten@wayfindergames.se - END

	bool ValidateGraph(const FAssetData& InAssetData, UBlueprint* Blueprint, UEdGraph* Graph, FDataValidationContext& Context)
	{
		bool bHasMultiPinPureNode = false;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node* PureNode = Cast<UK2Node>(Node);
			if (PureNode && PureNode->IsNodePure())
			{
				if (IsWhitelistedPureNode(PureNode))
				{
					continue;
				}

				const auto NodeMenuTitle = PureNode->GetNodeTitle(ENodeTitleType::Type::MenuTitle);
// WAYFINDER_CHANGE: kirsten@wayfindergames.se - BEGIN
				if (IsArrayOutputUsedAsMacroInput(PureNode))
				{
					FText Output = FText::Join(FText::FromString(" "), FText::FromString(InAssetData.AssetName.ToString()), NodeMenuTitle, FText::FromString(TEXT("We do not allow pure node array outputs as an input for macros.")));
					Context.AddError(Output);

					bHasMultiPinPureNode = true;
					continue;
				}

				auto CallFunctionNode = Cast<UK2Node_CallFunction>(Node);
				if (CallFunctionNode && CallFunctionNode->GetTargetFunction() && CallFunctionNode->GetTargetFunction()->HasAnyFunctionFlags(FUNC_Const | FUNC_Static))
				{
					// kirsten: ignore const functions for now
					continue;
				}
// WAYFINDER_CHANGE: kirsten@wayfindergames.se - END

				if (IsMultiPinPureNode(PureNode))
				{
					FText Output = FText::Join(FText::FromString(" "), FText::FromString(InAssetData.AssetName.ToString()), NodeMenuTitle, FText::FromString(" - "), FText::FromString(TEXT("MultiPin Pure Nodes actually get called for each connected pin output.")));

					PureNode->ErrorMsg = Output.ToString();
					PureNode->ErrorType = EMessageSeverity::Warning;
					PureNode->bHasCompilerMessage = true;

					// Create tokenized message with action using UCommonValidatorsStatics::OpenBlueprintAndFocusNode
					TSharedRef<FTokenizedMessage> TokenizedMessage = FTokenizedMessage::Create(EMessageSeverity::Warning, Output);
					TokenizedMessage->AddToken(FActionToken::Create(
						FText::FromString(TEXT("Open Blueprint and Focus Node")),
						FText::FromString(TEXT("Open Blueprint and Focus Node")),
						FOnActionTokenExecuted::CreateLambda([Blueprint, Graph, PureNode]()
							{
								UCommonValidatorsStatics::OpenBlueprintAndFocusNode(Blueprint, Graph, PureNode);
							}),
						false
					));

					Context.AddMessage(TokenizedMessage);

					bHasMultiPinPureNode = true;
					Graph->NotifyNodeChanged(Node);
				}
			}				
		}

		return bHasMultiPinPureNode;
	}
}

bool UEditorValidator_PureNode::CanValidateAsset_Implementation(const FAssetData& InAssetData, UObject* InObject, FDataValidationContext& InContext) const
{
	return InObject && InObject->IsA<UBlueprint>();
}

EDataValidationResult UEditorValidator_PureNode::ValidateLoadedAsset_Implementation(const FAssetData& InAssetData, UObject* InAsset, FDataValidationContext& Context)
{
	UBlueprint* Blueprint = Cast<UBlueprint>(InAsset);
	if (!Blueprint) return EDataValidationResult::NotValidated;

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (EditorValidator_PureNode::ValidateGraph(InAssetData, Blueprint, Graph, Context))
		{
			return EDataValidationResult::Invalid;
		}
	}

	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (EditorValidator_PureNode::ValidateGraph(InAssetData, Blueprint, Graph, Context))
		{
			return EDataValidationResult::Invalid;
		}
	}

	return EDataValidationResult::Valid;
}