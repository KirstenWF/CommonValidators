#include "EditorValidator_PureNode.h"

#include "Engine/Blueprint.h"
#include "Misc/DataValidation.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_Variable.h"
#include "CommonValidatorsStatics.h"
#include "CommonValidatorsDeveloperSettings.h"
#include "K2Node.h"
#include "K2Node_MacroInstance.h"

namespace UE::Internal::PureNodeValidatorHelpers
{
	bool IsStaticOrConstPureNode(UK2Node_CallFunction* CallNode)
	{
		if (!CallNode) return false;

		UFunction* Func = CallNode->GetTargetFunction();
		if (!Func)
		{
			return false;
		}

		return Func->HasAnyFunctionFlags(FUNC_Const | FUNC_Static);
	}

	bool IsHarmlessPureNode(UK2Node_CallFunction* CallNode)
	{
		if (!CallNode) return false;

		UFunction* Func = CallNode->GetTargetFunction();
		if (!Func)
		{
			return false;
		}

		if (Func->HasMetaData(TEXT("NativeBreakFunc")) || Func->HasMetaData(TEXT("NativeMakeFunc")))
		{
			return true;
		}

		UClass* OwnerClass = Func->GetOuterUClass();
		if (!OwnerClass)
		{
			return false;
		}

		if (GetDefault<UCommonValidatorsDeveloperSettings>()->HarmlessOwner.Contains(OwnerClass->GetName()))
		{
			return true;
		}

		return false;
	}

	bool IsWhitelistedPureNode(UEdGraphNode* CallNode)
	{
		if (GetDefault<UCommonValidatorsDeveloperSettings>()->WhitelistedPureNodes.Contains(CallNode->GetClass()->GetName()))
		{
			return true;
		}

		return false;
	}
	
    // Finds every event/function entry (no incoming exec) in the graph.
    static void CollectExecEntries(UEdGraph* Graph, TSet<UEdGraphNode*>& OutEntries)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input) == nullptr)
            {
                OutEntries.Add(Node);
            }
        }
    }

    // Marks all nodes reachable via exec pins from those entries.
    static void CollectReachableExecNodes(UEdGraph* Graph, TSet<UEdGraphNode*>& OutReachable)
    {
        TSet<UEdGraphNode*> Visited;
        TArray<UEdGraphNode*> Queue;

        CollectExecEntries(Graph, Visited);
        for (UEdGraphNode* Entry : Visited)
        {
            Queue.Add(Entry);
        }

        while (Queue.Num() > 0)
        {
            UEdGraphNode* Current = Queue.Pop();
            OutReachable.Add(Current);

            for (UEdGraphPin* Pin : Current->Pins)
            {
                if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Output)
                {
                    for (UEdGraphPin* Link : Pin->LinkedTo)
                    {
                        UEdGraphNode* Next = Link->GetOwningNode();
                        if (!Visited.Contains(Next))
                        {
                            Visited.Add(Next);
                            Queue.Add(Next);
                        }
                    }
                }
            }
        }
    }

    // Walks the data‑pin graph until the first connected exec‑input is found.
    static UEdGraphNode* FindFirstImpureSink(UEdGraphNode* StartNode)
    {
        TSet<UEdGraphNode*> Visited;
        TArray<UEdGraphNode*> Queue = { StartNode };

        while (Queue.Num() > 0)
        {
            UEdGraphNode* Current = Queue.Pop();
            if (Visited.Contains(Current))
            {
                continue;
            }
            Visited.Add(Current);

            if (UEdGraphPin* ExecIn = Current->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
            {
                if (ExecIn->LinkedTo.Num() > 0)
                {
                    return Current;
                }
            }

            for (UEdGraphPin* Pin : Current->Pins)
            {
                if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                {
                    for (UEdGraphPin* Link : Pin->LinkedTo)
                    {
                        Queue.Add(Link->GetOwningNode());
                    }
                    for (UEdGraphPin* Sub : Pin->SubPins)
                    {
                        for (UEdGraphPin* Link : Sub->LinkedTo)
                        {
                            Queue.Add(Link->GetOwningNode());
                        }
                    }
                }
            }
        }

        return nullptr;
    }

    bool WillPureNodeFireMultipleTimes(UK2Node* PureFunctionNode, UEdGraph* Graph)
    {
        TSet<UEdGraphNode*> Reachable;
        CollectReachableExecNodes(Graph, Reachable);

        TSet<UEdGraphNode*> DataConsumers;
        for (UEdGraphPin* Pin : PureFunctionNode->Pins)
        {
            if (Pin->Direction != EGPD_Output || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                continue;
            }

            for (UEdGraphPin* Link : Pin->LinkedTo)
            {
                DataConsumers.Add(Link->GetOwningNode());
            }
            for (UEdGraphPin* Sub : Pin->SubPins)
            {
                for (UEdGraphPin* Link : Sub->LinkedTo)
                {
                    DataConsumers.Add(Link->GetOwningNode());
                }
            }
        }

        TSet<UEdGraphNode*> ExecSinks;
        for (UEdGraphNode* Consumer : DataConsumers)
        {
            UEdGraphNode* Sink = FindFirstImpureSink(Consumer);
            if (Sink != nullptr && Reachable.Contains(Sink))
            {
                ExecSinks.Add(Sink);
                if (ExecSinks.Num() > 1)
                {
                    return true;
                }
            }
        }

        return false;
    }

	bool IsArrayOutputUsedAsMacroInput(const UEdGraphNode* PureNode)
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

	bool ValidateGraph(const FAssetData& InAssetData, UBlueprint* Blueprint, UEdGraph* Graph, FDataValidationContext& Context)
	{
		bool bFoundBadNode = false;
		bool bShouldError = GetDefault<UCommonValidatorsDeveloperSettings>()->bErrorOnPureNodeMultiExec;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node->IsA<UK2Node_BreakStruct>() || Node->IsA<UK2Node_Variable>())
			{
				continue;
			}

			if (IsWhitelistedPureNode(Node))
			{
				continue;
			}

			UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
			if (CallNode == nullptr)
			{
				continue;
			}

			if (UFunction* TargetFunc = CallNode->GetTargetFunction())
			{
				if (TargetFunc->HasMetaData(TEXT("NativeBreakFunc")) ||
					TargetFunc->HasMetaData(TEXT("NativeMakeFunc")))
				{
					continue;
				}
			}
			
			if (!CallNode->IsNodePure() || IsHarmlessPureNode(CallNode) || IsStaticOrConstPureNode(CallNode))
			{
				continue;
			}

			bool bIsArrayOutputUsedAsMacroInput = IsArrayOutputUsedAsMacroInput(Node);
			if (bIsArrayOutputUsedAsMacroInput || WillPureNodeFireMultipleTimes(CallNode, Graph))
			{
				const FText Title = CallNode->GetNodeTitle(ENodeTitleType::Type::MenuTitle);
				const FText ErrorMessage = bIsArrayOutputUsedAsMacroInput ? 
					FText::FromString(TEXT("pure array node is used as input for macros. Please cache the array first, because calling macro function.")) : 
					FText::FromString(TEXT("will execute more than once. Convert to exec or avoid using across multiple exec nodes."));

				const FText Message = IsRunningCommandlet() ?
					FText::Format(
						NSLOCTEXT("PureNodeValidator", "MultiCallWarning",
							"{0} - {1} {2}"),
						FText::FromName(InAssetData.AssetName),
						Title,
						ErrorMessage
					) : 
					FText::Format(
						NSLOCTEXT("PureNodeValidator", "MultiCallWarning",
							"{0} {1}."),
						Title,
						ErrorMessage
				);

				CallNode->ErrorMsg = Message.ToString();
				CallNode->ErrorType = bShouldError ? EMessageSeverity::Error : EMessageSeverity::Warning;
				CallNode->bHasCompilerMessage = true;

				TSharedRef<FTokenizedMessage> TokenMessage =
					FTokenizedMessage::Create(EMessageSeverity::Warning, Message);

				TokenMessage->AddToken(
					FActionToken::Create(
						NSLOCTEXT("PureNodeValidator", "OpenNode", "Focus Node"),
						NSLOCTEXT("PureNodeValidator", "OpenNodeTooltip", "Open this node in the Blueprint Editor"),
						FOnActionTokenExecuted::CreateLambda([Blueprint, Graph, CallNode]()
							{
								UCommonValidatorsStatics::OpenBlueprintAndFocusNode(Blueprint, Graph, CallNode);
							}),
						/*bEnabled=*/false
					)
				);

				Context.AddMessage(TokenMessage);
				Graph->NotifyNodeChanged(Node);
				bFoundBadNode = true;
			}
		}

		if (bShouldError && bFoundBadNode)
		{
			return false;
		}

		return true;
	}
} // namespace UE::Internal::PureNodeValidatorHelpers


bool UEditorValidator_PureNode::CanValidateAsset_Implementation(
    const FAssetData& InAssetData,
    UObject* InObject,
    FDataValidationContext& InContext) const
{
	bool bIsValidatorEnabled = GetDefault<UCommonValidatorsDeveloperSettings>()->bEnablePureNodeMultiExecValidator;
    return bIsValidatorEnabled && (InObject != nullptr) && InObject->IsA<UBlueprint>();
}

EDataValidationResult UEditorValidator_PureNode::ValidateLoadedAsset_Implementation(const FAssetData& InAssetData, UObject* InAsset, FDataValidationContext& Context)
{
	UBlueprint* Blueprint = Cast<UBlueprint>(InAsset);
	if (!Blueprint) return EDataValidationResult::NotValidated;

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (UE::Internal::PureNodeValidatorHelpers::ValidateGraph(InAssetData, Blueprint, Graph, Context) == false)
		{
			return EDataValidationResult::Invalid;
		}
	}

	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (UE::Internal::PureNodeValidatorHelpers::ValidateGraph(InAssetData, Blueprint, Graph, Context) == false)
		{
			return EDataValidationResult::Invalid;
		}
	}

	return EDataValidationResult::Valid;
}