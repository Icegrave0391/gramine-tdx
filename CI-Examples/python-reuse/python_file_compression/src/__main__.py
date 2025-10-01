import handler

def main(event_dict=None):
    if event_dict is None:
        event_dict = {}
    return handler.handler(event_dict, None)

if __name__ == "__main__":
    # Test the function
    result = main()
    print(result)
